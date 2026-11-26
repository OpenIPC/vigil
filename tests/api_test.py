#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""End-to-end test of vigild's HTTP/WebSocket API (docs/api.md).

    api_test.py VIGILD CLIP

Starts the NETIP camera emulator with CLIP and vigild (generic platform)
with a temporary configuration file, shadow file and free port, then
drives the API: authentication, status / config / schema, a camera added
with PUT that comes up live, invalid configurations refused with their
errors, password redaction, and the WebSocket's status, event and config
messages. Only the standard library: a minimal WebSocket client is below.
"""
import base64
import http.client
import json
import os
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
# openssl passwd -6 -salt vigiltestsalt secret
HASH = ("$6$vigiltestsalt$YNf0cVVTPqcmB27N7LaoJSiqJTsS7DvT6GejoILXrgfuBwdj8rCp9"
        "lnFrtSM5TO4kEOwYmcBLCsL4TzvT5U4b1")
CAM_PASS = "camsecret"

failures = []


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what, flush=True)
    if not cond:
        failures.append(what)
    return cond


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def wait_for(pred, secs, step=0.2):
    end = time.time() + secs
    while time.time() < end:
        v = pred()
        if v:
            return v
        time.sleep(step)
    return None


class Api:
    def __init__(self, port, user="root", password="secret"):
        self.base = "http://127.0.0.1:%d" % port
        self.auth = None
        if user is not None:
            tok = base64.b64encode(("%s:%s" % (user, password)).encode()).decode()
            self.auth = "Basic " + tok

    def call(self, method, path, body=None):
        data = None if body is None else (body if isinstance(body, bytes) else json.dumps(body).encode())
        req = urllib.request.Request(self.base + path, data=data, method=method)
        if self.auth:
            req.add_header("Authorization", self.auth)
        if data is not None:
            req.add_header("Content-Type", "application/json")
        try:
            with urllib.request.urlopen(req, timeout=10) as r:
                return r.status, dict(r.headers), r.read()
        except urllib.error.HTTPError as e:
            return e.code, dict(e.headers), e.read()

    def json(self, method, path, body=None):
        st, hdr, raw = self.call(method, path, body)
        try:
            doc = json.loads(raw) if raw else None
        except ValueError:
            doc = None
        return st, hdr, doc


class WebSocket:
    """Just enough RFC 6455 for a client: handshake, unmasked text frames
    from the server, pings answered, a reader thread collecting JSON."""

    def __init__(self, port, auth, origin=None, stalled=False):
        if stalled:
            # A client that never reads, with a tiny receive window: the
            # server's writes to it block once a few KiB are in flight.
            self.sock = socket.socket()
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
            self.sock.settimeout(10)
            self.sock.connect(("127.0.0.1", port))
        else:
            self.sock = socket.create_connection(("127.0.0.1", port), timeout=10)
        key = base64.b64encode(os.urandom(16)).decode()
        req = ("GET /api/v1/ws HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nUpgrade: websocket\r\n"
               "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n"
               "Authorization: %s\r\n%s\r\n" % (port, key, auth,
                                                  "Origin: %s\r\n" % origin if origin else ""))
        self.sock.sendall(req.encode())
        head = b""
        while b"\r\n\r\n" not in head:
            chunk = self.sock.recv(1)
            if not chunk:
                break
            head += chunk
        self.status = int(head.split(b" ", 2)[1]) if head.startswith(b"HTTP/1.1 ") else 0
        self.messages = []
        self.lock = threading.Lock()
        self.closed = False
        if self.status == 101 and not stalled:
            self.thread = threading.Thread(target=self._reader, daemon=True)
            self.thread.start()

    def _recv(self, n):
        buf = b""
        while len(buf) < n:
            chunk = self.sock.recv(n - len(buf))
            if not chunk:
                raise EOFError
            buf += chunk
        return buf

    def _send(self, opcode, payload=b""):
        mask = os.urandom(4)
        hdr = bytes([0x80 | opcode, 0x80 | len(payload)]) + mask
        self.sock.sendall(hdr + bytes(b ^ mask[i % 4] for i, b in enumerate(payload)))

    def _reader(self):
        self.sock.settimeout(None)
        try:
            while True:
                b0, b1 = self._recv(2)
                n = b1 & 0x7F
                if n == 126:
                    n = struct.unpack(">H", self._recv(2))[0]
                elif n == 127:
                    n = struct.unpack(">Q", self._recv(8))[0]
                payload = self._recv(n)
                op = b0 & 0x0F
                if op == 9:
                    self._send(10, payload)
                elif op == 8:
                    break
                elif op == 1:
                    with self.lock:
                        self.messages.append(json.loads(payload))
        except (EOFError, OSError):
            pass
        self.closed = True

    def find(self, pred):
        with self.lock:
            return [m for m in self.messages if pred(m)]

    def close(self):
        try:
            self._send(8, struct.pack(">H", 1000))
        except OSError:
            pass
        self.sock.close()


def main():
    vigild, clip = sys.argv[1], sys.argv[2]
    # In the build tree, not /tmp: recordings need a disk (tmpfs is refused),
    # and allow_rootfs lets the build tree be on the root filesystem.
    tmp = tempfile.mkdtemp(prefix="vigil-api-", dir=os.getcwd())
    api_port, cam_port = free_port(), free_port()
    shadow = os.path.join(tmp, "shadow")
    with open(shadow, "w") as f:
        f.write("root:%s:19000:0:99999:7:::\n" % HASH)
    cfg_path = os.path.join(tmp, "vigil.yaml")
    rec = os.path.join(tmp, "rec")
    # Written as a person would (a JSON string is a double-quoted YAML one).
    with open(cfg_path, "w") as f:
        f.write("# a comment of my own\n"
                "cameras: []\n"
                "record:\n"
                "  path: %s\n"
                "  segment: 60\n"
                "  keep: 3\n"
                "  allow_rootfs: yes\n"
                "api:\n"
                "  listen: 127.0.0.1:%d\n"
                "  auth: on\n"
                "  user: root\n"
                "  shadow: %s\n" % (json.dumps(rec), api_port, json.dumps(shadow)))

    def file_config():
        """The file as vigild reads it, through vigild -w into JSON."""
        out = os.path.join(tmp, "check.json")
        subprocess.run([vigild, "-f", cfg_path, "-w", out], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        with open(out) as f:
            return json.load(f)

    srv = subprocess.Popen([sys.executable, os.path.join(HERE, "netip_server.py"), clip,
                            "--port", str(cam_port), "--password", CAM_PASS],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    log_path = os.path.join(tmp, "vigild.log")
    log = open(log_path, "w")
    vd = subprocess.Popen([vigild, "-f", cfg_path, "-t", "90"], stdout=log, stderr=subprocess.STDOUT)
    ws = None
    try:
        ok = wait_for(lambda: "api: listening" in open(log_path).read(), 10)
        if not check(ok, "vigild serves the API"):
            return

        api = Api(api_port)
        # ---- authentication
        st, hdr, _ = Api(api_port, user=None).call("GET", "/api/v1/status")
        check(st == 401 and hdr.get("WWW-Authenticate", "").startswith("Basic"),
              "no credentials: 401 with WWW-Authenticate (%s)" % st)
        t0 = time.time()
        st, _, _ = Api(api_port, password="wrong").call("GET", "/api/v1/status")
        check(st == 401 and time.time() - t0 >= 0.9, "wrong password: 401, answered late (%s)" % st)
        st, _, _ = Api(api_port, user="admin").call("GET", "/api/v1/status")
        check(st == 401, "other account: 401 (%s)" % st)

        # ---- the three documents
        st, hdr, status = api.json("GET", "/api/v1/status")
        check(st == 200 and hdr.get("Content-Type") == "application/json", "status: 200 JSON")
        check(status and status["platform"]["name"] == "generic" and status["channels"] == [],
              "status: generic platform, no channels")
        st, _, cfg = api.json("GET", "/api/v1/config")
        check(st == 200 and cfg["version"] == 1 and cfg["record"]["path"] == rec and
              cfg["record"]["allow_rootfs"] is True and cfg["api"]["auth"] is True,
              "config: as in the YAML file")
        st, _, schema = api.json("GET", "/api/v1/schema")
        secs = {s["key"]: s for s in schema["sections"]} if st == 200 else {}
        seg = [f for f in secs.get("record", {}).get("fields", []) if f["key"] == "segment"]
        check(set(secs) == {"cameras", "record", "api"} and seg and seg[0]["min"] == 10 and
              seg[0]["max"] == 3600 and seg[0]["live"], "schema: sections and limits")
        st, _, _ = api.json("GET", "/api/v1/nothing")
        check(st == 404, "unknown endpoint: 404")
        st, hdr, _ = api.json("DELETE", "/api/v1/config")
        check(st == 405 and "PUT" in hdr.get("Allow", ""), "DELETE config: 405 with Allow")
        st, _, _ = api.json("PUT", "/api/v1/config/nope", {})
        check(st == 404, "unknown section: 404")

        # ---- WebSocket
        bad = WebSocket(api_port, "Basic " + base64.b64encode(b"root:nope").decode())
        check(bad.status == 401, "WebSocket with a wrong password: 401 (%s)" % bad.status)
        bad.sock.close()
        cross = WebSocket(api_port, api.auth, origin="http://evil.example")
        check(cross.status != 101, "WebSocket from a page elsewhere (Origin): refused (%s)" % cross.status)
        cross.sock.close()
        ws = WebSocket(api_port, api.auth, origin="http://127.0.0.1:%d" % api_port)
        check(ws.status == 101, "WebSocket (same Origin): 101 Switching Protocols")
        check(wait_for(lambda: ws.find(lambda m: m["type"] == "status"), 3),
              "WebSocket: a status right away")

        # ---- add a camera
        url = "netip://admin:%s@127.0.0.1:%d/" % (CAM_PASS, cam_port)
        st, _, body = api.json("PUT", "/api/v1/config/cameras", [{"url": url, "name": "Emulator"}])
        check(st == 200 and body[0]["url"] == "netip://admin:***@127.0.0.1:%d/" % cam_port,
              "PUT a camera: 200, password redacted in the answer")

        def live():
            s, _, d = api.json("GET", "/api/v1/status")
            ch = d["channels"] if s == 200 else []
            return ch and ch[0]["live"] and ch[0]["recording"] and d
        status = wait_for(live, 20, 0.5)
        if check(status, "the camera comes up live and recording"):
            ch = status["channels"][0]
            check(ch["name"] == "Emulator" and ch["host"] == "127.0.0.1" and ch["codec"] == "H.264" and
                  ch["width"] == 640 and CAM_PASS not in json.dumps(status),
                  "status: name, host, codec, size; no password")
            check(status["config"]["applied"] == status["config"]["generation"], "status: config applied")
        check(wait_for(lambda: ws.find(lambda m: m["type"] == "event" and m["data"]["camera"] == 1 and
                                       m["data"]["text"] == "online"), 5),
              "WebSocket: event CAM1 online")
        check(ws.find(lambda m: m["type"] == "event" and m["data"]["camera"] == 0 and
                      m["data"]["text"] == "config applied"), "WebSocket: event config applied")
        check(ws.find(lambda m: m["type"] == "config" and m["data"]["generation"] >= 1),
              "WebSocket: config generation message")
        check(wait_for(lambda: ws.find(lambda m: m["type"] == "status" and m["data"]["channels"] and
                                       m["data"]["channels"][0]["live"]), 3),
              "WebSocket: status with the channel live")

        # ---- redaction and the stored file
        st, _, cfg = api.json("GET", "/api/v1/config")
        check(cfg["cameras"][0]["url"].startswith("netip://admin:***@"), "GET config: password redacted")
        disk = open(cfg_path).read()
        check(CAM_PASS in disk and (os.stat(cfg_path).st_mode & 0o777) == 0o600,
              "the file keeps the password, mode 0600")
        check(disk.startswith("# vigild configuration") and "\ncameras:\n  - url: \"netip://admin:" in disk and
              "    name: Emulator\n" in disk and "\n  # Segment length (s): " in disk and
              "a comment of my own" not in disk,
              "the file is saved as YAML, with the settings' help as comments")
        check(file_config()["cameras"][0]["url"] == url, "and reads back as saved")
        gen = status["config"]["generation"] if status else 0
        events_before = len(ws.find(lambda m: m["type"] == "event"))
        cfg["cameras"][0]["name"] = "Gate"
        st, _, _ = api.json("PUT", "/api/v1/config", cfg)
        check(st == 200, "PUT the redacted config back with a new name: 200")
        check(CAM_PASS in open(cfg_path).read(), "*** kept the stored password")
        renamed = wait_for(lambda: api.json("GET", "/api/v1/status")[2]["channels"][0]["name"] == "Gate", 5)
        check(renamed, "the new name shows in the status")
        time.sleep(1)
        check(not ws.find(lambda m: m["type"] == "event" and m["data"]["text"] == "restarted"),
              "a rename does not restart the camera")
        check(len(ws.find(lambda m: m["type"] == "event")) > events_before, "rename applied (event)")
        st, _, d = api.json("GET", "/api/v1/status")
        check(d["config"]["generation"] > gen and d["channels"][0]["live"], "generation moved, still live")

        # ---- a recording setting restarts the camera, which comes back
        st, _, _ = api.json("PUT", "/api/v1/config/record", {"path": rec, "segment": 120, "allow_rootfs": True})
        check(st == 200 and wait_for(lambda: ws.find(lambda m: m["type"] == "event" and
                                                     m["data"]["text"] == "restarted"), 5),
              "record changed: CAM1 restarted")
        check(wait_for(live, 20, 0.5), "and live again")

        # ---- a recording directory that cannot be made yet is tried again
        blocker = os.path.join(tmp, "blocker")
        open(blocker, "w").close()                  # a file where a directory must go
        st, _, _ = api.json("PUT", "/api/v1/config/record", {"path": os.path.join(blocker, "rec"),
                                                             "allow_rootfs": True})

        def recording(want):
            def f():
                d = api.json("GET", "/api/v1/status")[2]
                ch = d["channels"]
                return ch and ch[0]["live"] and ch[0]["recording"] == want
            return f
        check(st == 200 and wait_for(recording(False), 10), "recording directory missing: live, not recording")
        os.unlink(blocker)
        check(wait_for(recording(True), 20, 0.5), "the directory can be made now: recording starts by itself")
        check(wait_for(lambda: ws.find(lambda m: m["type"] == "event" and m["data"]["text"] == "REC on"), 3),
              "WebSocket: event REC on")

        # ---- no disk where the path leads: nothing is written, the state says why
        def rec_state(want):
            def f():
                ch = api.json("GET", "/api/v1/status")[2]["channels"]
                return ch and ch[0]["live"] and not ch[0]["recording"] and ch[0]["rec_state"] == want
            return f
        onroot = "/vigil-test-%d-%d" % (os.getpid(), api_port)   # its parent is "/": the root filesystem
        st, _, body = api.json("PUT", "/api/v1/config/record", {"path": onroot + "/rec"})
        check(st == 200 and body["path"] == onroot + "/rec" and not body["allow_rootfs"],
              "record.path on the root filesystem: accepted, allow_rootfs off by default")
        check(wait_for(rec_state("waiting for disk"), 10), "status: live, not recording, rec_state waiting for disk")
        check(not os.path.exists(onroot), "nothing created on the root filesystem")
        check(wait_for(lambda: ws.find(lambda m: m["type"] == "event" and
                                       m["data"]["text"] == "REC waiting for disk"), 3),
              "WebSocket: event REC waiting for disk")
        check(api.json("GET", "/api/v1/config/record")[2]["path"] == onroot + "/rec" and
              file_config()["record"]["path"] == onroot + "/rec",
              "the configured path is kept, in force and in the file")
        if os.path.isdir("/dev/shm") and os.path.exists("/proc/self/mountinfo"):
            shm = "/dev/shm/vigil-test-%d-%d" % (os.getpid(), api_port)
            st, _, _ = api.json("PUT", "/api/v1/config/record", {"path": shm + "/rec", "allow_rootfs": True})
            check(st == 200 and wait_for(rec_state("not a disk"), 10) and not os.path.exists(shm),
                  "record.path in RAM (/dev/shm): not a disk, nothing created")
        st, _, err = api.json("PUT", "/api/v1/config/record", {"path": rec + "/../../escape"})
        check(st == 400 and err["errors"][0]["path"] == "record.path", "record.path with \"..\": 400")
        api.json("PUT", "/api/v1/config/record", {"path": rec, "segment": 120, "allow_rootfs": True})
        check(wait_for(recording(True), 20, 0.5) and
              api.json("GET", "/api/v1/status")[2]["channels"][0]["rec_state"] == "recording",
              "back on the build tree: recording, rec_state recording")

        # ---- a client that does not read: the others go on, shutdown does
        # not wait for it. (Smoke test: what the API sends in a test's time
        # fits in the kernel's socket buffers, so no write actually blocks.)
        stalled = WebSocket(api_port, api.auth, stalled=True)
        check(stalled.status == 101, "a second WebSocket client (it will not read)")
        st, _, cams = api.json("GET", "/api/v1/config/cameras")
        for i in range(100):
            cams[0]["name"] = "Gate %d" % i
            api.json("PUT", "/api/v1/config/cameras", cams)
        t0 = time.time()
        before = len(ws.find(lambda m: m["type"] == "status"))
        got = wait_for(lambda: len(ws.find(lambda m: m["type"] == "status")) >= before + 3, 8)
        check(got, "statuses keep coming to the other client (3 in %.1f s)" % (time.time() - t0))
        check(wait_for(lambda: ws.find(lambda m: m["type"] == "event" and m["data"]["text"] == "config applied"
                                       and m["data"]["seq"] > 0), 3) and
              wait_for(lambda: api.json("GET", "/api/v1/status")[2]["channels"][0]["name"] == "Gate 99", 5),
              "the configuration is applied meanwhile")

        # ---- invalid configurations change nothing
        before = open(cfg_path).read()
        st, _, err = api.json("PUT", "/api/v1/config",
                              {"record": {"segment": 5}, "cameras": [{"url": "http://x/"}], "bogus": 1})
        paths = sorted(e["path"] for e in (err or {}).get("errors", []))
        check(st == 400 and paths == ["bogus", "cameras[0].url", "record.segment"],
              "invalid config: 400 with every error (%s)" % paths)
        st, _, err = api.json("PUT", "/api/v1/config/record", b"{\"segment\": ")
        check(st == 400 and "JSON" in err["errors"][0]["message"], "bad JSON: 400")
        st, _, err = api.json("PUT", "/api/v1/config/cameras",
                              [{"url": "rtsp://admin:***@10.9.9.9/x"}])
        check(st == 400 and "does not match" in err["errors"][0]["message"],
              "*** for a camera never stored: 400")
        st, _, _ = api.json("PUT", "/api/v1/config", b"x" * (70 * 1024))
        check(st == 413, "body over 64 KiB: 413 (%s)" % st)
        check(open(cfg_path).read() == before, "the file is unchanged after the errors")

        # ---- remove the camera
        st, _, _ = api.json("PUT", "/api/v1/config/cameras", [])
        check(st == 200 and wait_for(lambda: api.json("GET", "/api/v1/status")[2]["channels"] == [], 5),
              "cameras removed")
        check(wait_for(lambda: ws.find(lambda m: m["type"] == "event" and m["data"]["text"] == "removed"), 3),
              "WebSocket: event removed")

        # ---- a second vigild cannot have the port: it runs on without the API
        busy = subprocess.run([vigild, "-f", cfg_path, "-o", "none", "-t", "2"],
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=20)
        check(busy.returncode == 0 and "running without the API" in busy.stdout,
              "port taken: logged, vigild runs on without the API (rc %s)" % busy.returncode)

        # ---- guessing: after 5 failures in a row an address is locked out,
        # and only that address: the owner elsewhere still gets in
        def from_addr(src, password):
            conn = http.client.HTTPConnection("127.0.0.1", api_port, timeout=10,
                                              source_address=(src, 0))
            tok = base64.b64encode(("root:%s" % password).encode()).decode()
            conn.request("GET", "/api/v1/status", headers={"Authorization": "Basic " + tok})
            r = conn.getresponse()
            r.read()
            conn.close()
            return r.status, r.getheader("Retry-After")
        threads = [threading.Thread(target=from_addr, args=("127.0.0.3", "guess")) for _ in range(6)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        st, ra = from_addr("127.0.0.3", "secret")
        check(st == 429 and int(ra or "0") >= 1,
              "6 failed logins from one address: even the right password from it gets 429 (%s)" % st)
        st, _ = from_addr("127.0.0.2", "secret")
        check(st == 200, "the owner on another address still gets in (%s)" % st)
        st, _, _ = api.call("GET", "/api/v1/status")
        check(st == 200, "and so does the already logged-in client (%s)" % st)
    finally:
        if ws:
            ws.close()
        # the stalled client stays connected: shutdown must not wait on it
        t_stop = time.time()
        vd.send_signal(signal.SIGTERM)
        try:
            rc = vd.wait(10)
            print("vigild stopped %.1f s after SIGTERM" % (time.time() - t_stop))
        except subprocess.TimeoutExpired:
            vd.kill()
            rc = None
        srv.terminate()
        srv.wait()
        log.close()
        text = open(log_path).read()
        print("---- vigild log\n" + text, end="")
        check(rc == 0, "vigild stops cleanly on SIGTERM (rc %s)" % rc)
        check(CAM_PASS not in text, "no camera password in the log")
        subprocess.call(["rm", "-rf", tmp])

    if failures:
        print("FAIL: %d check(s)" % len(failures))
        sys.exit(1)
    print("PASS")


if __name__ == "__main__":
    main()
