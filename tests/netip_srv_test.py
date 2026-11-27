#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""vigild's NETIP server end to end (src/api/netip_srv.c).

    netip_srv_test.py VIGILD H264_CLIP H265_CLIP

Two NETIP cameras (netip_server.py, one H.264, one H.265) behind a vigild
whose NETIP server is on; then, as XM's clients do:
  - logins: a wrong password (203, the connection ends), five of them
    locking the address out (205), vigil's user with the hash set with
    its password, the system account only once it logged in to the API;
  - the device's information, channel names, time, a configuration it
    does not have (607), a write (107, read-only), an unknown message
    (OK);
  - live view on the logged-in connection (python-dvr's way) and on a
    second connection claiming the session (CMS's way, with the start
    answered on the first one), start before claim, stop, and the media
    connection ending with its session; no live permission: 107;
  - the frames: XM headers with the stream's codec and size, Annex-B
    payloads; and vigil's own NETIP source recording both cameras from
    this vigild, which ffmpeg decodes;
  - the LAN search (UDP 34569) answered with the server's port.
"""
import base64
import hashlib
import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
# crypt("secret", "$6$vigiltestsalt$"), as api_test.py
HASH = ("$6$vigiltestsalt$YNf0cVVTPqcmB27N7LaoJSiqJTsS7DvT6GejoILXrgfuBwdj8rCp9"
        "lnFrtSM5TO4kEOwYmcBLCsL4TzvT5U4b1")
HDR = struct.Struct("<BB2xIIBBHI")
failures = []


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what, flush=True)
    if not cond:
        failures.append(what)
    return cond


def sofia(password):
    d = hashlib.md5(password.encode()).digest()
    chars = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
    return "".join(chars[(d[2 * i] + d[2 * i + 1]) % 62] for i in range(8))


def free_port(kind=socket.SOCK_STREAM):
    s = socket.socket(socket.AF_INET, kind)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class Conn:
    def __init__(self, port, src=None):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=5,
                                          source_address=(src, 0) if src else None)
        self.session = 0
        self.seq = 0

    def send(self, msgid, body, session=None):
        data = json.dumps(body).encode() + b"\x0a\x00"
        sess = self.session if session is None else session
        self.s.sendall(HDR.pack(0xFF, 0, sess, self.seq, 0, 0, msgid, len(data)) + data)
        self.seq += 1

    def exact(self, n):
        buf = b""
        while len(buf) < n:
            part = self.s.recv(n - len(buf))
            if not part:
                raise ConnectionError("closed")
            buf += part
        return buf

    def msg(self):
        head, ver, sess, seq, b12, b13, msgid, length = HDR.unpack(self.exact(20))
        return msgid, self.exact(length), b12

    def reply(self, msgid, body):
        self.send(msgid, body)
        while True:
            rid, data, _ = self.msg()
            if rid != 1412:
                return rid, json.loads(data.rstrip(b"\x00\x0a") or b"{}")

    def login(self, user, password=None, xm=None):
        rid, r = self.reply(1000, {"EncryptType": "MD5", "LoginType": "DVRIP-Web", "UserName": user,
                                   "PassWord": xm if xm is not None else sofia(password)})
        if r.get("Ret") == 100:
            self.session = int(r["SessionID"], 16)
        return rid, r

    def get(self, msgid, name):
        return self.reply(msgid, {"Name": name, "SessionID": "0x%08X" % self.session})

    def frames(self, count, secs=10):
        """`count` frames of the media: (kind, codec byte, w, h, payload)."""
        out, cur = [], None
        end = time.time() + secs
        self.s.settimeout(secs)
        while len(out) < count and time.time() < end:
            rid, data, _ = self.msg()
            if rid != 1412:
                continue
            if cur is None:
                tag = struct.unpack(">I", data[:4])[0]
                if tag == 0x1FC:
                    codec, fps, w, h, dt, length = struct.unpack("<BBBBII", data[4:16])
                    w |= (codec >> 4 & 3) << 8
                    h |= (codec >> 6 & 3) << 8
                    cur = ["I", codec & 15, w * 8, h * 8, data[16:], length]
                elif tag == 0x1FD:
                    cur = ["P", 0, 0, 0, data[8:], struct.unpack("<I", data[4:8])[0]]
                else:
                    raise ValueError("tag %x" % tag)
            else:
                cur[4] += data
            if len(cur[4]) >= cur[5]:
                out.append(tuple(cur[:5]))
                cur = None
        return out

    def close(self):
        self.s.close()


def closed(conn, secs=3):
    conn.s.settimeout(secs)
    try:
        while True:
            if not conn.s.recv(65536):
                return True
    except (socket.timeout, OSError):
        return False


def monitor_req(session, action, ch, stream):
    return {"Name": "OPMonitor", "SessionID": "0x%08X" % session,
            "OPMonitor": {"Action": action, "Parameter": {"Channel": ch, "CombinMode": "NONE",
                                                          "StreamType": stream, "TransMode": "TCP"}}}


def main():
    vigild, h264, h265 = sys.argv[1:4]
    tmp = tempfile.mkdtemp(prefix="vigil-netips-", dir=os.getcwd())
    procs = []
    try:
        run(vigild, h264, h265, tmp, procs)
    finally:
        for p in procs:
            p.terminate()
        for p in procs:
            try:
                p.wait(5)
            except subprocess.TimeoutExpired:
                p.kill()
    if failures:
        print("FAIL: %d check(s)" % len(failures))
        log = os.path.join(tmp, "a.log")
        if os.path.exists(log):
            print(open(log).read())
        sys.exit(1)
    shutil.rmtree(tmp, ignore_errors=True)
    print("PASS")


def run(vigild, h264, h265, tmp, procs):
    cam = [free_port(), free_port()]
    api_port, port = free_port(), free_port()
    for clip, p in ((h264, cam[0]), (h265, cam[1])):
        procs.append(subprocess.Popen([sys.executable, os.path.join(HERE, "netip_server.py"), clip, "--port",
                                       str(p), "--password", "cam", "--title", "Gate"],
                                      stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT))
    with open(os.path.join(tmp, "shadow"), "w") as f:
        f.write("root:%s:19000:0:99999:7:::\n" % HASH)
    users = os.path.join(tmp, "users.yaml")
    with open(users, "w") as f:
        f.write("groups:\n  - name: blind\n    live: false\n    playback: true\n"
                "users:\n"
                "  - name: viewer\n    group: user\n    password: \"%s\"\n    netip_password: \"%s\"\n"
                "  - name: nolive\n    group: blind\n    password: \"%s\"\n    netip_password: \"%s\"\n"
                "  - name: old\n    group: user\n    password: \"%s\"\n" % (HASH, sofia("secret"), HASH,
                                                                          sofia("secret"), HASH))
    os.chmod(users, 0o600)
    cfg = os.path.join(tmp, "a.yaml")
    with open(cfg, "w") as f:
        f.write("cameras:\n  - url: \"netip://admin:cam@127.0.0.1:%d/\"\n  - url: \"netip://admin:cam@127.0.0.1:%d/\"\n"
                "api:\n  listen: \"127.0.0.1:%d\"\n  shadow: \"%s/shadow\"\n  users: \"%s\"\n"
                "web:\n  enabled: false\nnetip_server:\n  port: %d\n" % (cam[0], cam[1], api_port, tmp, users, port))
    log = open(os.path.join(tmp, "a.log"), "w")
    procs.append(subprocess.Popen([vigild, "-f", cfg, "-t", "90"], stdout=log, stderr=subprocess.STDOUT))

    def up():
        try:
            c = Conn(port)
            c.close()
            return True
        except OSError:
            return False
    end = time.time() + 10
    while time.time() < end and not up():
        time.sleep(0.2)
    check(up(), "the NETIP server listens")

    # ---- logins
    c = Conn(port)
    rid, r = c.login("viewer", "wrong")
    check(rid == 1001 and r.get("Ret") == 203, "wrong password: 1001 Ret 203 (%s)" % r)
    check(closed(c), "... and the connection ends")
    c = Conn(port)
    rid, r = c.login("nobody", "secret")
    check(r.get("Ret") == 203, "unknown user: Ret 203, the same (%s)" % r)
    c = Conn(port)
    rid, r = c.login("old", "secret")
    check(r.get("Ret") == 203, "a user with no NETIP hash yet: refused (%s)" % r)
    c = Conn(port)
    rid, r = c.login("root", "secret")
    check(r.get("Ret") == 203, "the system account before it logged in anywhere: refused (%s)" % r)

    m = Conn(port)
    rid, r = m.login("viewer", "secret")
    check(rid == 1001 and r.get("Ret") == 100 and r.get("ChannelNum") == 2 and "DeviceType " in r and
          r.get("AliveInterval") == 20 and m.session, "viewer logs in: ChannelNum 2, \"DeviceType \", session (%s)" % r)

    # The API login lets vigil keep the system account's password, sealed,
    # and "old"'s.
    for user in ("root", "old"):
        req = urllib.request.Request("http://127.0.0.1:%d/api/v1/me" % api_port)
        req.add_header("Authorization", "Basic " + base64.b64encode(("%s:secret" % user).encode()).decode())
        try:
            st = urllib.request.urlopen(req, timeout=5).status
        except Exception as e:  # noqa: BLE001
            st = str(e)
        check(st == 200, "%s logs in to the API (%s)" % (user, st))
    for user in ("root", "old"):
        c = Conn(port)
        rid, r = c.login(user, "secret")
        check(r.get("Ret") == 100, "%s logs in over NETIP after its API login (%s)" % (user, r))
        c.close()
    with open(users) as f:
        text = f.read()
    check(text.count(sofia("secret")) == 2 and text.count('password_enc: "enc:v1:') == 2,
          "root's and old's passwords kept sealed in the users file, the others' old hashes as they were")

    # ---- information
    rid, r = m.get(1020, "SystemInfo")
    si = r.get("SystemInfo", {})
    check(rid == 1021 and r.get("Ret") == 100 and si.get("DigChannel") == 2 and si.get("VideoInChannel") == 0 and
          si.get("HardWare", "").startswith("OpenIPC") and len(si.get("SerialNo", "")) == 16,
          "SystemInfo: 2 digital channels, OpenIPC, a serial (%s)" % si)
    end = time.time() + 10
    titles = []
    while time.time() < end and titles != ["Gate", "Gate"]:
        rid, r = m.get(1048, "ChannelTitle")
        titles = r.get("ChannelTitle")
        time.sleep(0.3)
    check(rid == 1049 and titles == ["Gate", "Gate"], "ChannelTitle: the cameras' names (%s)" % titles)
    rid, r = m.get(1042, "ChannelTitle")
    check(rid == 1043 and r.get("ChannelTitle") == ["Gate", "Gate"], "ChannelTitle by config get too")
    rid, r = m.get(1452, "OPTimeQuery")
    check(rid == 1453 and len(r.get("OPTimeQuery", "")) == 19, "OPTimeQuery: the time (%s)" % r)
    rid, r = m.get(1042, "General.General")
    check(r.get("Ret") == 100 and "MachineName" in r.get("General.General", {}), "General.General")
    rid, r = m.get(1360, "SystemFunction")
    check(r.get("Ret") == 100 and r["SystemFunction"]["EncodeFunction"]["DoubleStream"], "SystemFunction")
    rid, r = m.get(1042, "Simplify.Encode")
    enc = r.get("Simplify.Encode") or [{}, {}]
    check(r.get("Ret") == 100 and enc[1].get("MainFormat", {}).get("Video", {}).get("Compression") == "H.265",
          "Simplify.Encode: the second camera's H.265 (%s)" % enc[1])
    rid, r = m.get(1042, "Detect")
    check(rid == 1043 and r.get("Ret") == 607, "a configuration it does not have: 607")
    rid, r = m.reply(1040, {"Name": "General.General", "SessionID": "0x%08X" % m.session, "General.General": {}})
    check(rid == 1041 and r.get("Ret") == 107, "a write: 107, read-only (%s)" % r)
    rid, r = m.reply(1999, {"Name": "Whatever", "SessionID": "0x%08X" % m.session})
    check(rid == 2000 and r.get("Ret") == 100, "an unknown message: OK (%s)" % r)
    rid, r = m.get(1006, "KeepAlive")
    check(rid == 1007 and r.get("Ret") == 100, "keepalive")

    # ---- live on the logged-in connection (python-dvr, vigil)
    rid, r = m.reply(1413, monitor_req(m.session, "Claim", 0, "Main"))
    check(rid == 1414 and r.get("Ret") == 100, "claim on the logged-in connection")
    m.send(1410, monitor_req(m.session, "Start", 0, "Main"))
    fr = m.frames(30)
    check(len(fr) == 30 and fr[0][0] == "I" and fr[0][1] == 2 and fr[0][2:4] == (640, 360) and
          fr[0][4].startswith(b"\x00\x00\x00\x01"),
          "media on it at once: an H.264 640x360 I-frame first, Annex-B, then 29 more (%d)" % len(fr))
    m.close()

    # ---- live on a second connection (CMS, XMEye)
    a = Conn(port)
    a.login("viewer", "secret")
    b = Conn(port)
    b.session = a.session
    rid, r = b.reply(1413, monitor_req(a.session, "Claim", 1, "Extra1"))
    check(rid == 1414 and r.get("Ret") == 100, "claim on a new connection with the session's id")
    rid, r = a.reply(1410, monitor_req(a.session, "Start", 1, "Extra1"))
    check(rid == 1411 and r.get("Ret") == 100, "the start answered on the logged-in connection")
    fr = b.frames(10)
    check(len(fr) == 10 and fr[0][0] == "I" and fr[0][1] == 3, "H.265 media on the claiming connection")
    rid, r = a.reply(1410, monitor_req(a.session, "Stop", 1, "Extra1"))
    check(rid == 1411 and r.get("Ret") == 100 and closed(b), "stop: answered, the media connection closes")
    # Start first, then the claim.
    rid, r = a.reply(1410, monitor_req(a.session, "Start", 0, "Extra1"))
    b = Conn(port)
    b.session = a.session
    rid, r = b.reply(1413, monitor_req(a.session, "Claim", 0, "Extra1"))
    fr = b.frames(5)
    check(len(fr) == 5 and fr[0][0] == "I", "a start before its claim: the media comes all the same")
    a.reply(1002, {"Name": "", "SessionID": "0x%08X" % a.session})
    check(closed(b), "logout: the session's media connection ends")
    b = Conn(port)
    rid, r = b.reply(1413, monitor_req(0x7777, "Claim", 0, "Main"))
    check(r.get("Ret") == 105 and closed(b), "a claim for no session: 105, closed")
    a = Conn(port)
    a.login("viewer", "secret")
    a2 = Conn(port)
    a2.login("viewer", "secret")
    check(a.session and a2.session and abs(a.session - a2.session) > 1 and a.session < 0x80000000,
          "session ids: random, 31 bits (0x%08X, 0x%08X)" % (a.session, a2.session))
    a2.close()
    b = Conn(port, src="127.0.0.2")
    rid, r = b.reply(1413, monitor_req(a.session, "Claim", 0, "Main"))
    check(r.get("Ret") == 105 and closed(b), "a claim of a live session from another address: 105, closed")
    a.close()

    n = Conn(port)
    rid, r = n.login("nolive", "secret")
    rid, r = n.reply(1413, monitor_req(n.session, "Claim", 0, "Main"))
    check(r.get("Ret") == 107, "no live permission: the claim gets 107 (%s)" % r)
    n.close()

    # ---- the LAN search, from a port of its own: answered back there
    u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    u.bind(("127.0.0.1", 0))
    u.settimeout(3)
    u.sendto(struct.pack("<BB2xIIBBHI", 0xFF, 0, 0, 0, 0, 0, 1530, 0), ("127.0.0.1", 34569))
    got = None
    try:
        while True:
            data, _ = u.recvfrom(2048)
            body = json.loads(data[20:].rstrip(b"\x00\x0a"))
            if body.get("NetWork.NetCommon", {}).get("TCPPort") == port:
                got = body["NetWork.NetCommon"]
                break
    except (socket.timeout, ValueError):
        pass
    if "cannot answer searches" in open(os.path.join(tmp, "a.log")).read():
        print("skip the search: UDP 34569 is taken on this host")
    else:
        check(got is not None and got.get("HostIP") == "0x0100007F" and len(data) <= 1024,
              "a search: answered with this server's port and address, within 1 KiB (%s)" % got)

    # ---- vigil's camera search: this vigild does not list itself, another does
    def api(port_, method, path, body=None):
        req = urllib.request.Request("http://127.0.0.1:%d%s" % (port_, path), method=method,
                                     data=None if body is None else json.dumps(body).encode())
        req.add_header("Authorization", "Basic " + base64.b64encode(b"root:secret").decode())
        try:
            with urllib.request.urlopen(req, timeout=5) as r:
                return r.status, json.loads(r.read() or b"{}")
        except urllib.error.HTTPError as e:
            return e.code, {}
        except OSError:
            return 0, {}

    def scan(api_port_):
        st, _ = api(api_port_, "POST", "/api/v1/discovery/scan",
                    {"protocols": "netip", "targets": "127.0.0.1", "broadcast": False, "seconds": 2})
        time.sleep(2.8)
        st2, doc = api(api_port_, "GET", "/api/v1/discovery")
        return st, [d for d in doc.get("devices", []) if d.get("ports", {}).get("netip") == port]
    searchable = "cannot answer searches" not in open(os.path.join(tmp, "a.log")).read()
    st, mine = scan(api_port)
    check(st == 202 and not mine, "its own search does not list itself (%s %s)" % (st, mine))
    c_api = free_port()
    ccfg = os.path.join(tmp, "c.yaml")
    with open(ccfg, "w") as f:
        f.write("api:\n  listen: \"127.0.0.1:%d\"\n  shadow: \"%s/shadow\"\n  users: \"%s/c-users.yaml\"\n"
                "web:\n  enabled: false\nnetip_server:\n  enabled: false\n" % (c_api, tmp, tmp))
    other = subprocess.Popen([vigild, "-f", ccfg, "-t", "20"], stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    procs.append(other)
    end = time.time() + 10
    while time.time() < end and api(c_api, "GET", "/api/v1/me")[0] != 200:
        time.sleep(0.2)
    st, theirs = scan(c_api)
    if searchable:
        check(st == 202 and len(theirs) == 1 and theirs[0].get("channels") == 2,
              "another vigild's search lists it, with its 2 channels (%s)" % theirs)
    other.terminate()

    # ---- vigil's own NETIP source plays it, both cameras, both codecs
    rec = os.path.join(tmp, "rec")
    bcfg = os.path.join(tmp, "b.yaml")
    with open(bcfg, "w") as f:
        f.write("record:\n  allow_rootfs: true\napi:\n  listen: \"\"\nweb:\n  enabled: false\n"
                "netip_server:\n  enabled: false\n")
    out = subprocess.run([vigild, "-t", "7", "-f", bcfg, "-S", "60", "-o", rec,
                          "-c", "netip://viewer:secret@127.0.0.1:%d/?ch=0" % port,
                          "-c", "netip://viewer:secret@127.0.0.1:%d/?ch=1" % port],
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=30).stdout
    files = sorted(os.listdir(rec)) if os.path.isdir(rec) else []
    check(len(files) == 2 and files[0].endswith(".h264") and files[1].endswith(".h265"),
          "vigil's NETIP source records both: %s" % files)
    check('the camera calls it "Gate"' in out, "... and takes the names")
    if shutil.which("ffprobe"):
        for name in files:
            p = os.path.join(rec, name)
            info = subprocess.run(["ffprobe", "-v", "error", "-count_frames", "-show_entries",
                                   "stream=codec_name,nb_read_frames", "-of", "csv=p=0", p],
                                  stdout=subprocess.PIPE, text=True).stdout.strip()
            errs = subprocess.run(["ffmpeg", "-v", "error", "-i", p, "-f", "null", "-"],
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True).stdout.strip()
            frames = int(info.split(",")[-1]) if info else 0
            check(frames >= 50 and not errs, "%s decodes: %s, errors %r" % (name, info, errs[:200]))

    # ---- five wrong logins lock the address out, even for the right one
    for _ in range(5):
        c = Conn(port)
        c.login("viewer", "nope")
        c.close()
    c = Conn(port)
    rid, r = c.login("viewer", "secret")
    check(r.get("Ret") == 205, "after five wrong logins: locked out, 205 (%s)" % r)

    # ---- the port moves at once; a taken one is refused before it is saved
    taken = socket.socket()
    taken.bind(("0.0.0.0", 0))
    taken.listen(1)
    st, _ = api(api_port, "PUT", "/api/v1/config/netip_server",
                {"enabled": True, "port": taken.getsockname()[1]})
    check(st == 400, "a port another program holds: refused (%s)" % st)
    taken.close()
    new = free_port()
    st, _ = api(api_port, "PUT", "/api/v1/config/netip_server", {"enabled": True, "port": new})
    moved = False
    end = time.time() + 5
    while time.time() < end and not moved:
        try:
            Conn(new).close()
            moved = True
        except OSError:
            time.sleep(0.2)
    try:
        Conn(port).close()
        old_open = True
    except OSError:
        old_open = False
    check(st == 200 and moved and not old_open, "moved to port %d at once, the old one closed (%s)" % (new, st))
    st, _ = api(api_port, "PUT", "/api/v1/config/netip_server", {"enabled": False, "port": new})
    time.sleep(1.5)
    try:
        Conn(new).close()
        off = False
    except OSError:
        off = True
    check(st == 200 and off, "turned off: nothing listens (%s)" % st)


if __name__ == "__main__":
    main()
