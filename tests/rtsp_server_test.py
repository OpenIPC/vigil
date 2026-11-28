#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""End-to-end test of vigild's RTSP server (docs/rtsp.md).

    rtsp_server_test.py VIGILD DATA_DIR

Two NETIP camera emulators (H.264 with a sub-stream, H.265) feed a vigild
whose RTSP server is on a free port, with Digest logins against a users
file. Checked: the challenge, a user with the live permission let in, one
without it and a wrong password refused, the first packet after PLAY being
a keyframe at once (the GOP replay), both codecs over TCP and UDP with
ffprobe, the Hikvision and Dahua paths, max_clients, and a second vigild
recording the first one's stream through its own RTSP client (decoded
with ffmpeg).
"""
import hashlib
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
# openssl passwd -6 -salt vigiltestsalt secret
HASH = ("$6$vigiltestsalt$YNf0cVVTPqcmB27N7LaoJSiqJTsS7DvT6GejoILXrgfuBwdj8rCp9"
        "lnFrtSM5TO4kEOwYmcBLCsL4TzvT5U4b1")
REALM = "vigil"

failures = []


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what, flush=True)
    if not cond:
        failures.append(what)
    return cond


def md5(s):
    return hashlib.md5(s.encode()).hexdigest()


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class Rtsp:
    """Just enough of an RTSP client over TCP, with Digest."""

    def __init__(self, port, user=None, password=None):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=5)
        self.buf = b""
        self.cseq = 0
        self.user, self.password = user, password
        self.challenge = None

    def request(self, method, url, extra=""):
        self.cseq += 1
        head = "%s %s RTSP/1.0\r\nCSeq: %d\r\n%s" % (method, url, self.cseq, extra)
        if self.challenge and self.user is not None:
            ha1 = md5("%s:%s:%s" % (self.user, REALM, self.password))
            resp = md5("%s:%s:%s" % (ha1, self.challenge, md5("%s:%s" % (method, url))))
            head += ('Authorization: Digest username="%s", realm="%s", nonce="%s", uri="%s", '
                     'response="%s"\r\n' % (self.user, REALM, self.challenge, url, resp))
        self.s.sendall((head + "\r\n").encode())
        code, hdr, body = self.reply()
        m = re.search(r'WWW-Authenticate: Digest realm="([^"]*)", nonce="([^"]*)"', hdr)
        if code == 401 and m and self.user is not None and not self.challenge:
            self.challenge = m.group(2)
            return self.request(method, url, extra)
        return code, hdr, body

    def reply(self):
        while True:
            self.skip_packets()
            i = self.buf.find(b"\r\n\r\n")
            if i >= 0 and not self.buf.startswith(b"$"):
                hdr = self.buf[:i].decode()
                cl = re.search(r"Content-Length: *(\d+)", hdr, re.I)
                n = i + 4 + (int(cl.group(1)) if cl else 0)
                while len(self.buf) < n:
                    self.buf += self.s.recv(65536)
                body = self.buf[i + 4:n].decode(errors="replace")
                self.buf = self.buf[n:]
                return int(hdr.split()[1]), hdr, body
            d = self.s.recv(65536)
            if not d:
                raise EOFError
            self.buf += d

    def skip_packets(self):
        while self.buf.startswith(b"$") and len(self.buf) >= 4:
            n = 4 + (self.buf[2] << 8 | self.buf[3])
            if len(self.buf) < n:
                return
            self.buf = self.buf[n:]

    def packet(self, timeout):
        """The next interleaved packet (channel, payload), or None."""
        end = time.time() + timeout
        while time.time() < end:
            if self.buf.startswith(b"$") and len(self.buf) >= 4:
                n = 4 + (self.buf[2] << 8 | self.buf[3])
                if len(self.buf) >= n:
                    ch, p = self.buf[1], self.buf[4:n]
                    self.buf = self.buf[n:]
                    return ch, p
            try:
                d = self.s.recv(65536)
            except socket.timeout:
                continue
            if not d:
                return None
            self.buf += d
        return None

    def play(self, url):
        code, hdr, _ = self.request("DESCRIBE", url, "Accept: application/sdp\r\n")
        if code != 200:
            return code
        code, hdr, _ = self.request("SETUP", url + "/trackID=0",
                                    "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n")
        if code != 200:
            return code
        self.session = re.search(r"Session: *([^;\r\n]+)", hdr).group(1)
        code, _, _ = self.request("PLAY", url, "Session: %s\r\n" % self.session)
        return code

    def close(self):
        self.s.close()


def is_key(payload, h265):
    p = payload[12:]
    if h265:
        t = (p[0] >> 1) & 0x3f
        if t == 49:
            t = p[2] & 0x3f
        return 16 <= t <= 21 or t in (32, 33, 34)
    t = p[0] & 0x1f
    if t == 28:
        t = p[1] & 0x1f
    return t in (5, 7, 8)


def ffprobe(url, transport):
    r = subprocess.run(["ffprobe", "-v", "error", "-rtsp_transport", transport, "-show_entries",
                        "stream=codec_name,width,height", "-of", "csv=p=0", url],
                       capture_output=True, text=True, timeout=30)
    return r.stdout.strip() if r.returncode == 0 else "error: " + r.stderr.strip()[-200:]


def main():
    vigild, data = sys.argv[1], sys.argv[2]
    tmp = tempfile.mkdtemp(prefix="vigil-rtsps-", dir=os.getcwd())
    procs = []
    try:
        run(vigild, data, tmp, procs)
    finally:
        for p in procs:
            p.terminate()
        for p in procs:
            try:
                p.wait(10)
            except subprocess.TimeoutExpired:
                p.kill()
        shutil.rmtree(tmp, ignore_errors=True)
    print("%d failure(s)" % len(failures))
    return 1 if failures else 0


def run(vigild, data, tmp, procs):
    cams = []
    for clip in ("testsrc_640x360.h264", "majestic_1080p.h265"):
        port = free_port()
        procs.append(subprocess.Popen([sys.executable, os.path.join(HERE, "netip_server.py"),
                                       os.path.join(data, clip), "--port", str(port),
                                       "--password", "camsecret"],
                                      stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
        cams.append(port)
    with open(os.path.join(tmp, "shadow"), "w") as f:
        f.write("root:%s:19000:0:99999:7:::\n" % HASH)
    with open(os.path.join(tmp, "users.yaml"), "w") as f:
        f.write("groups:\n  - name: noview\n    playback: true\n"
                "users:\n"
                "  - name: anna\n    password: \"%s\"\n    digest: \"%s\"\n"
                "  - name: bob\n    group: noview\n    password: \"%s\"\n    digest: \"%s\"\n"
                % (HASH, md5("anna:%s:secret" % REALM), HASH, md5("bob:%s:secret" % REALM)))
    os.chmod(os.path.join(tmp, "users.yaml"), 0o600)
    port = free_port()
    cfg = os.path.join(tmp, "vigil.yaml")
    with open(cfg, "w") as f:
        f.write("cameras:\n"
                "  - url: netip://admin:camsecret@127.0.0.1:%d/\n"
                "  - url: netip://admin:camsecret@127.0.0.1:%d/\n"
                "record:\n  path: \"\"\n"
                "api:\n  listen: \"\"\n  shadow: %s\n  users: %s\n"
                "web:\n  enabled: false\n"
                "rtsp_server:\n  port: %d\n  max_clients: 3\n"
                % (cams[0], cams[1], os.path.join(tmp, "shadow"), os.path.join(tmp, "users.yaml"), port))
    log = open(os.path.join(tmp, "vigild.log"), "w")
    procs.append(subprocess.Popen([vigild, "-f", cfg], stdout=log, stderr=subprocess.STDOUT))
    base = "rtsp://127.0.0.1:%d" % port

    # Up when both cameras' streams are known.
    ready = False
    end = time.time() + 15
    while time.time() < end and not ready:
        try:
            ok = 0
            for path in ("/ch1/main", "/ch1/sub", "/ch2/main"):
                c = Rtsp(port, "anna", "secret")
                ok += c.request("DESCRIBE", base + path)[0] == 200
                c.close()
            ready = ok == 3
        except OSError:
            pass
        if not ready:
            time.sleep(0.3)
    if not check(ready, "vigild serves CH1 main and sub and CH2 over RTSP"):
        print(open(os.path.join(tmp, "vigild.log")).read())
        return

    c = Rtsp(port)
    code, hdr, _ = c.request("OPTIONS", base + "/ch1/main")
    check(code == 200 and "DESCRIBE" in hdr and "GET_PARAMETER" in hdr, "OPTIONS without a login")
    code, hdr, _ = c.request("DESCRIBE", base + "/ch1/main")
    check(code == 401 and 'Digest realm="vigil", nonce="' in hdr and "Basic" not in hdr,
          "DESCRIBE without a login: 401, a Digest challenge only")
    c.close()
    c = Rtsp(port, "anna", "wrong")
    check(c.request("DESCRIBE", base + "/ch1/main")[0] == 401, "a wrong password: 401")
    c.close()
    c = Rtsp(port, "bob", "secret")
    check(c.request("DESCRIBE", base + "/ch1/main")[0] == 403, "a user without the live permission: 403")
    c.close()
    c = Rtsp(port, "anna", "secret")
    check(c.request("DESCRIBE", base + "/ch9/main")[0] == 404, "a channel with no camera: 404")
    c.close()

    for path, h265, codec in (("/ch1/main", False, "H264"), ("/ch2/main", True, "H265")):
        c = Rtsp(port, "anna", "secret")
        code, _, sdp = c.request("DESCRIBE", base + path)
        check(code == 200 and "a=rtpmap:96 %s/90000" % codec in sdp and
              ("sprop-vps=" in sdp if h265 else "sprop-parameter-sets=" in sdp),
              "%s: SDP with %s and its parameter sets" % (path, codec))
        c.close()
        c = Rtsp(port, "anna", "secret")
        t0 = time.time()
        code = c.play(base + path)
        pkt = c.packet(2.0)
        dt = time.time() - t0
        check(code == 200 and pkt and pkt[0] == 0 and is_key(pkt[1], h265) and dt < 1.0,
              "%s: the first packet after PLAY is the keyframe, %.0f ms (a 1 s GOP)" % (path, dt * 1000))
        seqs = []
        end = time.time() + 1.5
        while time.time() < end:
            p = c.packet(0.5)
            if p and p[0] == 0:
                seqs.append(p[1][2] << 8 | p[1][3])
        check(len(seqs) > 20 and all((b - a) & 0xffff == 1 for a, b in zip(seqs, seqs[1:])),
              "%s: %d packets in sequence" % (path, len(seqs)))
        c.close()

    # UDP to the last port: no port left for RTCP after it.
    c = Rtsp(port, "anna", "secret")
    c.request("DESCRIBE", base + "/ch1/sub")
    code, _, _ = c.request("SETUP", base + "/ch1/sub/trackID=0", "Transport: RTP/AVP;unicast;client_port=65535\r\n")
    code2, hdr, _ = c.request("SETUP", base + "/ch1/sub/trackID=0",
                              "Transport: RTP/AVP;unicast;client_port=65535,RTP/AVP;unicast;client_port=50000\r\n")
    check(code == 461 and code2 == 200 and "client_port=50000-50001" in hdr,
          "UDP client_port=65535 alone: 461; the next offer taken (%d, %d)" % (code, code2))
    c.close()

    # max_clients: 3
    held = []
    for i in range(3):
        c = Rtsp(port, "anna", "secret")
        held.append((c, c.play(base + "/ch1/sub")))
    c = Rtsp(port, "anna", "secret")
    c.request("DESCRIBE", base + "/ch1/sub")
    code, _, _ = c.request("SETUP", base + "/ch1/sub/trackID=0",
                           "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n")
    check(all(r == 200 for _, r in held) and code == 453,
          "rtsp_server.max_clients 3: the fourth client is refused (453)")
    c.close()
    for c, _ in held:
        c.close()

    if shutil.which("ffprobe"):
        url = "rtsp://anna:secret@127.0.0.1:%d" % port
        for path, transport, want in (("/ch1/main", "tcp", "h264,640,360"), ("/ch1/sub", "udp", "h264,640,360"),
                                      ("/ch2/main", "udp", "hevc,1920,1080"), ("/ch2/main", "tcp", "hevc,1920,1080"),
                                      ("/Streaming/Channels/102", "tcp", "h264,640,360"),
                                      ("/cam/realmonitor?channel=2&subtype=0", "tcp", "hevc,1920,1080")):
            got = ffprobe(url + path, transport)
            check(got == want, "ffprobe %s over %s: %s" % (path, transport.upper(), got))
        got = ffprobe("rtsp://anna:nope@127.0.0.1:%d/ch1/main" % port, "tcp")
        check(got.startswith("error") and "401" in got, "ffprobe with a wrong password: refused")
    else:
        print("skip ffprobe: not installed")

    # vigild's own RTSP client, recording vigild's RTSP server.
    rec = os.path.join(tmp, "rec")
    cfg2 = os.path.join(tmp, "vigil2.yaml")
    with open(cfg2, "w") as f:
        f.write("record:\n  allow_rootfs: true\n  chunk_mb: 4\n  pool_mb: 16\napi:\n  listen: \"\"\n"
                "web:\n  enabled: false\nrtsp_server:\n  enabled: false\n")
    r = subprocess.run([vigild, "-t", "6", "-f", cfg2, "-S", "60", "-o", rec, "-c",
                        "rtsp://anna:secret@127.0.0.1:%d/ch1/main" % port],
                       capture_output=True, text=True, timeout=30)
    rc = subprocess.run([sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), "rec_check.py"),
                         rec, "--codec", "h264"], capture_output=True, text=True)
    if not check(rc.returncode == 0, "a second vigild records it over RTSP, and it decodes: %s" % rc.stdout.strip()):
        print(r.stderr[-2000:])
    log.flush()
    text = open(os.path.join(tmp, "vigild.log")).read()
    check("rtsp: 127.0.0.1 anna plays CH1 main over TCP" in text and "login failed from 127.0.0.1" in text,
          "the log says who played what, and the failed login")


if __name__ == "__main__":
    sys.exit(main())
