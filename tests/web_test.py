#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The zero channel's web server end to end (src/api/web.c, docs/api.md).

    web_test.py TEST_KVM [--shots DIR]

Runs `test_kvm --serve` (a small LVGL UI on an in-memory display, the real
src/app/kvm.c and web server) and checks, over HTTP and a WebSocket client
written here: logging in (none, wrong, a user without the remote
permission, another site's form, the rate limit), the session cookie, the
WebSocket handshake and its framing, the pictures (decoded with PIL when it
is installed) and the input reaching the UI. The WebSocket client is also
what tools/ use against a recorder (kvm_client below).
"""
import base64
import hashlib
import http.client as httpc
import io
import os
import queue
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

try:
    from PIL import Image
except ImportError:                     # decoding checks are skipped
    Image = None

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
FAILS = 0


def check(cond, what):
    global FAILS
    print(("ok   " if cond else "FAIL ") + what, flush=True)
    if not cond:
        FAILS += 1


# ---- HTTP --------------------------------------------------------------------------------

def http(port, method, path, body=None, headers=None):
    c = httpc.HTTPConnection("127.0.0.1", port, timeout=20)
    h = {"Host": "127.0.0.1:%d" % port}
    h.update(headers or {})
    if body is not None:
        h.setdefault("Content-Type", "application/x-www-form-urlencoded")
    c.request(method, path, body=body, headers=h)
    r = c.getresponse()
    data = r.read()
    c.close()
    return r.status, {k: v for k, v in r.getheaders()}, data


def login(port, user, password, origin=None):
    from urllib.parse import urlencode
    h = {"Origin": origin} if origin else {}
    return http(port, "POST", "/login", urlencode({"user": user, "password": password}), h)


# ---- WebSocket (RFC 6455, the client side) --------------------------------------------------------

class WsError(Exception):
    pass


class Ws:
    """A blocking WebSocket client: the handshake (its Sec-WebSocket-Accept
    checked), masked frames out, frames in (fragments joined, pings
    answered)."""

    def __init__(self, host, port, path="/ws", cookie=None, origin=None, timeout=20):
        self.s = socket.create_connection((host, port), timeout=timeout)
        key = base64.b64encode(os.urandom(16)).decode()
        req = ["GET %s HTTP/1.1" % path, "Host: %s:%d" % (host, port), "Upgrade: websocket",
               "Connection: Upgrade", "Sec-WebSocket-Key: " + key, "Sec-WebSocket-Version: 13"]
        if cookie:
            req.append("Cookie: " + cookie)
        if origin:
            req.append("Origin: " + origin)
        self.s.sendall(("\r\n".join(req) + "\r\n\r\n").encode())
        head = b""
        while b"\r\n\r\n" not in head:
            b = self.s.recv(4096)
            if not b:
                break
            head += b
        head, _, self.buf = head.partition(b"\r\n\r\n")
        lines = head.decode(errors="replace").split("\r\n")
        self.status = int(lines[0].split()[1]) if lines and len(lines[0].split()) > 1 else 0
        self.headers = {l.split(":", 1)[0].lower(): l.split(":", 1)[1].strip() for l in lines[1:] if ":" in l}
        want = base64.b64encode(hashlib.sha1((key + GUID).encode()).digest()).decode()
        self.accept_ok = self.headers.get("sec-websocket-accept") == want

    def _read(self, n):
        while len(self.buf) < n:
            b = self.s.recv(65536)
            if not b:
                raise WsError("closed")
            self.buf += b
        d, self.buf = self.buf[:n], self.buf[n:]
        return d

    def send(self, data, op=None):
        if isinstance(data, str):
            data, op = data.encode(), op or 1
        op = op or 2
        n = len(data)
        hdr = bytes([0x80 | op])
        if n < 126:
            hdr += bytes([0x80 | n])
        elif n < 65536:
            hdr += bytes([0x80 | 126]) + struct.pack(">H", n)
        else:
            hdr += bytes([0x80 | 127]) + struct.pack(">Q", n)
        mask = os.urandom(4)
        self.s.sendall(hdr + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))

    def recv(self):
        """(opcode, payload) of the next message; text as str."""
        msg, first = b"", None
        while True:
            b0, b1 = self._read(2)
            fin, op, n = b0 & 0x80, b0 & 0x0f, b1 & 0x7f
            if b1 & 0x80:
                raise WsError("a server frame must not be masked")
            if n == 126:
                n = struct.unpack(">H", self._read(2))[0]
            elif n == 127:
                n = struct.unpack(">Q", self._read(8))[0]
            data = self._read(n)
            if op == 9:                 # ping
                self.send(data, 10)
                continue
            if op == 10:
                continue
            if op == 8:
                return 8, data
            if op:
                first = op
            msg += data
            if fin:
                return first, msg.decode() if first == 1 else msg

    def close(self):
        try:
            self.send(b"", 8)
        except OSError:
            pass
        self.s.close()


def parse_picture(b):
    """The header and parts of a picture message (src/app/kvm_enc.h)."""
    if len(b) < 12 or b[0:1] != b"F":
        return None
    flags, w, h, n, seq = b[1], *struct.unpack("<HHHI", b[2:12])
    parts, off = [], 12
    for _ in range(n):
        x, y, pw, ph, ln = struct.unpack("<HHHHI", b[off:off + 12])
        parts.append((x, y, pw, ph, b[off + 12:off + 12 + ln]))
        off += 12 + ln
    return {"full": flags & 1, "w": w, "h": h, "seq": seq, "parts": parts, "end": off}


class Screen:
    """What a viewer has: pictures painted over each other as they come."""

    def __init__(self):
        self.img = None

    def paint(self, pic):
        if Image is None:
            return
        if self.img is None or self.img.size != (pic["w"], pic["h"]):
            self.img = Image.new("RGB", (pic["w"], pic["h"]))
        for x, y, w, h, jpg in pic["parts"]:
            part = Image.open(io.BytesIO(jpg))
            part.load()
            assert part.size == (w, h), (part.size, w, h)
            self.img.paste(part.convert("RGB"), (x, y))


class Viewer(threading.Thread):
    """Reads a zero channel's WebSocket, acking every picture."""

    def __init__(self, ws):
        super().__init__(daemon=True)
        self.ws, self.screen, self.q = ws, Screen(), queue.Queue()
        self.pics, self.texts, self.bad = 0, [], []
        self.lock = threading.Lock()

    def run(self):
        try:
            while True:
                op, data = self.ws.recv()
                if op == 8:
                    break
                if op == 1:
                    self.texts.append(data)
                    self.q.put(("text", data))
                    continue
                pic = parse_picture(data)
                if not pic or pic["end"] != len(data):
                    self.bad.append(len(data))
                    continue
                with self.lock:
                    try:
                        self.screen.paint(pic)
                    except Exception as e:      # noqa: BLE001 - recorded as a failure
                        self.bad.append(repr(e))
                    self.pics += 1
                self.q.put(("pic", pic))
                self.ws.send('{"t":"ack","seq":%d}' % pic["seq"])
        except (WsError, OSError):
            pass

    def wait(self, what, secs=10, pred=lambda x: True):
        end = time.time() + secs
        while time.time() < end:
            try:
                kind, x = self.q.get(timeout=max(0.01, end - time.time()))
            except queue.Empty:
                break
            if kind == what and pred(x):
                return x
        return None

    def pixel(self, x, y):
        with self.lock:
            return self.screen.img.getpixel((x, y)) if self.screen.img else None


# ---- the test --------------------------------------------------------------------------------------

def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def crypt_hash(pw):
    out = subprocess.run(["openssl", "passwd", "-6", pw], capture_output=True, text=True)
    return out.stdout.strip() if out.returncode == 0 else None


def near(c, want, tol=40):
    return c is not None and all(abs(a - b) <= tol for a, b in zip(c, want))


def main():
    exe = sys.argv[1]
    root_hash, user_hash = crypt_hash("testpw"), crypt_hash("userpw")
    if not root_hash:
        print("skip: no openssl to make password hashes")
        return 77
    tmp = tempfile.mkdtemp(prefix="vigil-web-")
    port = free_port()
    with open(tmp + "/shadow", "w") as f:
        f.write("root:%s:19000:0:99999:7:::\n" % root_hash)
    with open(tmp + "/users.yaml", "w") as f:
        f.write("groups:\n  - name: ops\n    live: true\n    remote: true\nusers:\n"
                "  - name: viewer\n    group: user\n    password: \"%s\"\n"
                "  - name: op\n    group: ops\n    password: \"%s\"\n" % (user_hash, user_hash))
    os.chmod(tmp + "/users.yaml", 0o600)
    with open(tmp + "/vigil.yaml", "w") as f:
        f.write("version: 1\napi:\n  listen: \"\"\n  shadow: %s/shadow\n  users: %s/users.yaml\n"
                "web:\n  listen: \"127.0.0.1:%d\"\n  fps: 20\n" % (tmp, tmp, port))
    p = subprocess.Popen([exe, "--serve", tmp + "/vigil.yaml"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, text=True, bufsize=1)
    said = queue.Queue()

    def reader():
        for line in p.stdout:
            said.put(line.rstrip("\n"))
    threading.Thread(target=reader, daemon=True).start()

    def expect(line, secs=5):
        end = time.time() + secs
        while time.time() < end:
            try:
                got = said.get(timeout=max(0.01, end - time.time()))
            except queue.Empty:
                break
            if got == line:
                return True
        return False

    try:
        check(expect("ready", 15), "test_kvm serves")
        # ---- pages and logging in
        st, hdr, _ = http(port, "GET", "/")
        check(st == 302 and hdr.get("Location") == "/login", "no session: / redirects to /login (%s)" % st)
        st, hdr, body = http(port, "GET", "/login")
        check(st == 200 and b'action="/login"' in body and hdr.get("X-Frame-Options") == "DENY" and
              "frame-ancestors 'none'" in hdr.get("Content-Security-Policy", ""),
              "login page, not to be framed")
        st, hdr, body = http(port, "GET", "/kvm.js")
        check(st == 200 and hdr.get("Content-Type", "").startswith("text/javascript") and b"WebSocket" in body,
              "the page's script")
        st, _, _ = http(port, "GET", "/session")
        check(st == 401, "no session: /session is 401")
        ws = Ws("127.0.0.1", port)
        check(ws.status == 401, "no session: the WebSocket is refused with 401 (%s)" % ws.status)
        ws.s.close()
        t0 = time.time()
        st, _, body = login(port, "root", "wrong")
        check(st == 401 and time.time() - t0 >= 0.9 and b"Wrong user name or password" in body,
              "wrong password: 401, answered late, said on the page")
        st, _, body = login(port, "viewer", "userpw")
        check(st == 403 and b"remote permission" in body, "a user without the remote permission: 403 (%s)" % st)
        st, _, _ = login(port, "root", "testpw", origin="http://evil.example")
        check(st == 403, "a login posted from another site: 403 (%s)" % st)
        st, hdr, _ = login(port, "op", "userpw")
        check(st == 303 and "vigil_zc=" in hdr.get("Set-Cookie", ""), "a user of a group with remote gets in")
        st, hdr, _ = login(port, "root", "testpw", origin="http://127.0.0.1:%d" % port)
        sc = hdr.get("Set-Cookie", "")
        cookie = sc.split(";")[0]
        check(st == 303 and hdr.get("Location") == "/" and "HttpOnly" in sc and "SameSite=Strict" in sc and
              len(cookie) == len("vigil_zc=") + 64, "root logs in: 303 to /, an HttpOnly SameSite cookie")
        st, _, body = http(port, "GET", "/", headers={"Cookie": cookie})
        check(st == 200 and b"/kvm.js" in body, "with the cookie: the viewer page")
        st, _, body = http(port, "GET", "/session", headers={"Cookie": cookie})
        check(st == 200 and b'"root"' in body, "with the cookie: /session names the user")
        st, _, _ = http(port, "GET", "/", headers={"Cookie": "vigil_zc=" + "0" * 64})
        check(st == 302, "a made-up cookie: back to /login")
        ws = Ws("127.0.0.1", port, cookie=cookie, origin="http://evil.example")
        check(ws.status == 403, "the WebSocket from another site's page: 403 (%s)" % ws.status)
        ws.s.close()

        # ---- the WebSocket, the pictures
        ws = Ws("127.0.0.1", port, cookie=cookie, origin="http://127.0.0.1:%d" % port)
        check(ws.status == 101 and ws.accept_ok, "handshake: 101, Sec-WebSocket-Accept right")
        op, hello = ws.recv()
        check(op == 1 and '"hello"' in hello and '"width":1280' in hello and '"height":720' in hello,
              "hello: the screen's size (%s)" % hello)
        v = Viewer(ws)
        v.start()
        pic = v.wait("pic", 10)
        check(pic and pic["full"] and pic["w"] == 1280 and pic["h"] == 720 and len(pic["parts"]) == 1,
              "the first picture: whole, 1280x720")
        if Image is not None and pic:
            time.sleep(0.3)
            check(near(v.pixel(50, 600), (0x20, 0x28, 0x30), 12), "the UI's background where it is (%s)" %
                  (v.pixel(50, 600),))
            px = v.pixel(1260, 340)
            check(px is not None and not near(px, (255, 0, 255), 60) and px[2] > px[0],
                  "the video in the UI's hole, not the colour key (%s)" % (px,))
        # video moves: pictures keep coming, each only the tile
        t0, n0 = time.time(), v.pics
        p2 = v.wait("pic", 5)
        check(p2 and not p2["full"] and all(x >= 640 and y < 360 for x, y, *_ in p2["parts"]),
              "then only the moving tile")
        time.sleep(2)
        fps = (v.pics - n0) / (time.time() - t0)
        check(fps >= 8, "pictures at %.1f a second with the video moving (fps 20 set)" % fps)
        st = v.wait("text", 3, lambda t: '"stats"' in t)
        check(st is not None and '"kbps"' in st, "stats once a second (%s)" % st)
        t0 = time.time()
        ws.send('{"t":"ping","c":12.5}')
        pong = v.wait("text", 3, lambda t: '"pong"' in t)
        check(pong is not None and "12.5" in pong, "ping answered (%.0f ms)" % ((time.time() - t0) * 1000))

        # ---- input
        def send(o):
            ws.send(o)
            time.sleep(0.05)
        send('{"t":"move","x":200,"y":130}')
        send('{"t":"btn","b":0,"down":true}')
        send('{"t":"btn","b":0,"down":false}')
        check(expect("clicks 1"), "a click on the button")
        for code, val in (("ShiftLeft", 1), ("KeyH", 1), ("KeyH", 0), ("ShiftLeft", 0), ("KeyI", 1), ("KeyI", 0)):
            send('{"t":"key","code":"%s","v":%d}' % (code, val))
        check(expect("text H") and expect("text Hi"), "typed into the text field")
        send('{"t":"move","x":700,"y":480}')
        send('{"t":"btn","b":2,"down":true}')
        send('{"t":"btn","b":2,"down":false}')
        check(expect("menu 1"), "the right button: the UI's menu")
        send('{"t":"wheel","n":-1}')
        check(expect("wheel -1"), "the wheel")
        if Image is not None:
            time.sleep(1)
            check(near(v.pixel(800, 600), (0x16, 0x23, 0x3a), 14), "the menu shows on the picture (%s)" %
                  (v.pixel(800, 600),))
            if len(sys.argv) > 3 and sys.argv[2] == "--shots":
                with v.lock:
                    v.screen.img.save(os.path.join(sys.argv[3], "web_test_screen.png"))
        ws.send('{"t":"opts","scale":2,"quality":50}')
        half = v.wait("pic", 5, lambda x: x["w"] == 640)
        check(half and half["full"] and half["h"] == 360, "half size asked: whole pictures at 640x360")
        check(not v.bad, "every picture well formed (%s)" % v.bad[:3])

        # ---- two viewers at most
        ws2 = Ws("127.0.0.1", port, cookie=cookie)
        ws3 = Ws("127.0.0.1", port, cookie=cookie)
        check(ws2.status == 101 and ws3.status == 503, "a third viewer is refused (%s %s)" % (ws2.status, ws3.status))
        ws3.s.close()
        ws2.close()
        ws.close()
        time.sleep(0.5)

        # ---- logging out (elsewhere: an open viewer is closed too)
        ws = Ws("127.0.0.1", port, cookie=cookie)
        v = Viewer(ws)
        v.start()
        st, hdr, _ = http(port, "POST", "/logout", "", {"Cookie": cookie})
        check(st == 303 and hdr.get("Location") == "/login" and "Max-Age=0" in hdr.get("Set-Cookie", ""),
              "logout: 303 to /login, the cookie cleared")
        err = v.wait("text", 4, lambda t: '"error"' in t)
        check(err is not None and "session ended" in err, "... and the viewer still open is told and closed (%s)" % err)
        v.join(5)
        check(not v.is_alive(), "... its WebSocket closed")
        ws.s.close()
        ws = Ws("127.0.0.1", port, cookie=cookie)
        check(ws.status == 401, "after logout the old cookie opens nothing (%s)" % ws.status)
        ws.s.close()

        # ---- the rate limit (last: it locks 127.0.0.1 out)
        codes = [login(port, "root", "wrong%d" % i)[0] for i in range(6)]
        st, hdr, body = login(port, "root", "testpw")
        check(codes[:5] == [401] * 5 and st == 429 and int(hdr.get("Retry-After", "0")) > 0 and b"try again" in body,
              "after 5 failures from one address even the right password gets 429 (%s, %s)" % (codes, st))
    finally:
        try:
            p.stdin.close()
            p.wait(timeout=10)
        except (subprocess.TimeoutExpired, OSError):
            p.kill()
    print("%d failed" % FAILS)
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
