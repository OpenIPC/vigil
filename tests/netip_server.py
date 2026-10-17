#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Minimal NETIP (Xiongmai DVRIP) camera emulator for vigil's tests.

Serves an Annex-B H.264/H.265 file in a loop as live video:
login (msgid 1000, "sofia" password hash), keepalive (1006 -> 1007),
OPMonitor Claim (1413 -> 1414) and Start (1410), then media as msgid 1412
messages carrying 0x1FC (I-frame) / 0x1FD (P-frame) frames, split over
several messages like real devices do.

    netip_server.py FILE [--port 34567] [--user admin] [--password ''] [--fps 25]
                    [--chatty] [--stall] [--video-frames N]

Fault modes for tests:
  --chatty          never answer the login; send unrelated messages instead
  --stall           after Start, send one media header and part of its body,
                    then nothing
  --video-frames N  after N video frames, send only audio (0x1FA) chunks
"""
import argparse
import hashlib
import json
import socket
import struct
import sys
import threading
import time

HDR = struct.Struct("<BB2xII2xHI")
CHUNK = 8192


def sofia(password):
    d = hashlib.md5(password.encode()).digest()
    chars = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
    return "".join(chars[(d[2 * i] + d[2 * i + 1]) % 62] for i in range(8))


def nal_units(data):
    """Split Annex-B into NAL units (without start codes)."""
    i, n, starts = 0, len(data), []
    while i + 3 <= n:
        if data[i] == 0 and data[i + 1] == 0 and data[i + 2] == 1:
            starts.append(i + 3)
            i += 3
        else:
            i += 1
    out = []
    for k, s in enumerate(starts):
        e = starts[k + 1] - 3 if k + 1 < len(starts) else n
        while e > s and data[e - 1] == 0:
            e -= 1
        out.append(data[s:e])
    return out


def access_units(data, h265):
    """Group NAL units into frames: non-VCL units go with the next picture."""
    aus, cur = [], []
    for nal in nal_units(data):
        t = (nal[0] >> 1) & 0x3F if h265 else nal[0] & 0x1F
        vcl = t < 32 if h265 else 1 <= t <= 5
        key = (16 <= t <= 21) if h265 else t == 5
        cur.append(nal)
        if vcl:
            aus.append((key, b"".join(b"\x00\x00\x00\x01" + x for x in cur)))
            cur = []
    return aus


class Client(threading.Thread):
    def __init__(self, sock, args, aus, h265):
        super().__init__(daemon=True)
        self.s, self.args, self.aus, self.h265 = sock, args, aus, h265
        self.session, self.streaming = 0x1234ABCD, False
        self.lock = threading.Lock()

    def send(self, msgid, body, raw=False):
        if not raw:
            body = json.dumps(body).encode() + b"\x0a\x00"
        with self.lock:
            self.s.sendall(HDR.pack(0xFF, 1, self.session, 0, msgid, len(body)) + body)

    def recv_exact(self, n):
        buf = b""
        while len(buf) < n:
            part = self.s.recv(n - len(buf))
            if not part:
                raise ConnectionError
            buf += part
        return buf

    def stream(self):
        fps = self.args.fps
        w, h = self.args.width // 8, self.args.height // 8
        if self.args.stall:
            with self.lock:
                self.s.sendall(HDR.pack(0xFF, 1, self.session, 0, 1412, 1000) + b"\0" * 10)
            while self.streaming:
                time.sleep(0.5)
            return
        t_next = time.monotonic()
        sent = 0
        while self.streaming:
            for key, au in self.aus:
                if not self.streaming:
                    return
                if self.args.video_frames and sent >= self.args.video_frames:
                    audio = struct.pack(">I", 0x1FA) + struct.pack("<BBH", 0xE, 2, 320) + b"\xd5" * 320
                    self.send(1412, audio, raw=True)
                    time.sleep(0.04)
                    continue
                sent += 1
                if key:
                    head = struct.pack(">I", 0x1FC) + struct.pack("<BBBBII", 3 if self.h265 else 2, fps, w, h, 0, len(au))
                else:
                    head = struct.pack(">I", 0x1FD) + struct.pack("<I", len(au))
                payload = head + au
                for off in range(0, len(payload), CHUNK):
                    self.send(1412, payload[off:off + CHUNK], raw=True)
                t_next += 1.0 / fps
                time.sleep(max(0.0, t_next - time.monotonic()))

    def run(self):
        try:
            while True:
                head, ver, sess, seq, msgid, length = HDR.unpack(self.recv_exact(20))
                body = self.recv_exact(length)
                req = json.loads(body.rstrip(b"\x00\x0a") or b"{}")
                if msgid == 1000 and self.args.chatty:
                    while True:
                        self.send(1007, {"Ret": 100, "Name": "KeepAlive"})
                        time.sleep(0.2)
                if msgid == 1000:
                    ok = req.get("UserName") == self.args.user and req.get("PassWord") == sofia(self.args.password)
                    self.send(1001, {"Ret": 100, "SessionID": "0x%08X" % self.session,
                                     "AliveInterval": 20, "DeviceType ": "IPC"} if ok else {"Ret": 203})
                    if not ok:
                        return
                elif msgid == 1006:
                    self.send(1007, {"Ret": 100, "SessionID": "0x%08X" % self.session})
                elif msgid == 1413:
                    self.send(1414, {"Ret": 100, "SessionID": "0x%08X" % self.session})
                elif msgid == 1410 and not self.streaming:
                    self.streaming = True
                    threading.Thread(target=self.stream, daemon=True).start()
        except (ConnectionError, OSError, ValueError):
            pass
        finally:
            self.streaming = False
            self.s.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    ap.add_argument("--port", type=int, default=34567)
    ap.add_argument("--user", default="admin")
    ap.add_argument("--password", default="")
    ap.add_argument("--fps", type=int, default=25)
    ap.add_argument("--width", type=int, default=640)
    ap.add_argument("--height", type=int, default=360)
    ap.add_argument("--chatty", action="store_true")
    ap.add_argument("--stall", action="store_true")
    ap.add_argument("--video-frames", type=int, default=0)
    args = ap.parse_args()
    h265 = args.file.endswith((".h265", ".hevc", ".265"))
    aus = access_units(open(args.file, "rb").read(), h265)
    if not aus:
        sys.exit("no frames in " + args.file)
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", args.port))
    srv.listen(8)
    print("netip_server: %d frames on :%d" % (len(aus), args.port), flush=True)
    while True:
        c, _ = srv.accept()
        Client(c, args, aus, h265).start()


if __name__ == "__main__":
    main()
