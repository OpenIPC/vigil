#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""A fake camera's snapshots, for vigil's snapshot tests: ONVIF's
GetSnapshotUri and the JPEG behind it, and a plain snapshot URL.

    fake_onvif_snap.py [--host 127.0.0.1] [--port 8899] [--user admin]
                       [--password ''] [--snapshot FILE] [--no-snapshot-uri]
                       [--advertise HOST:PORT]

Over HTTP:
  POST /onvif/device_service  GetSystemDateAndTime (no login), GetCapabilities
                              (Media: XAddr /onvif/media_service)
  POST /onvif/media_service   GetProfiles (main, sub), GetSnapshotUri: an
                              address with an escaped "&" (XM's style:
                              /webcapture.jpg?command=snap&channel=1), or the
                              ONVIF fault ActionNotSupported with
                              --no-snapshot-uri
SOAP calls carry a checked WS-UsernameToken (as fake_onvif_events.py's).
  GET /webcapture.jpg         the JPEG, behind HTTP Digest (MD5, qop auth)
  GET /cgi-bin/snapshot.cgi   the JPEG, behind HTTP Basic
The addresses it gives are http://HOST:PORT, or --advertise's (a camera
reached through a forward, as from an emulator's guest). Prints one line
per call.
"""
import argparse
import base64
import hashlib
import os
import re
import secrets
import socketserver
import sys
import time
from http.server import BaseHTTPRequestHandler, HTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
NS = ('xmlns:s="http://www.w3.org/2003/05/soap-envelope" xmlns:tt="http://www.onvif.org/ver10/schema" '
      'xmlns:tds="http://www.onvif.org/ver10/device/wsdl" xmlns:trt="http://www.onvif.org/ver10/media/wsdl"')
SOAP = '<?xml version="1.0" encoding="UTF-8"?><s:Envelope ' + NS + '><s:Body>%s</s:Body></s:Envelope>'
REALM = "fakecam"
COUNTS = {"digest": 0, "basic": 0, "uri": 0}


def log(*a):
    print("fake-snap:", *a, flush=True)


def xml_text(body, local):
    m = re.search(r"<(?:\w+:)?%s(?:\s[^>]*)?>(.*?)</(?:\w+:)?%s>" % (local, local), body, re.S)
    return m.group(1).strip() if m else ""


def md5(s):
    return hashlib.md5(s.encode()).hexdigest()


class Server(socketserver.ThreadingMixIn, HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def send(self, status, ctype, raw, extra=()):
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(raw)))
        for k, v in extra:
            self.send_header(k, v)
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(raw)
        self.close_connection = True

    def reply(self, status, inner):
        self.send(status, "application/soap+xml; charset=utf-8", (SOAP % inner).encode())

    def fault(self, code, text, status=400):
        self.reply(status, '<s:Fault><s:Code><s:Value>s:Sender</s:Value><s:Subcode><s:Value>%s</s:Value>'
                           '</s:Subcode></s:Code><s:Reason><s:Text xml:lang="en">%s</s:Text></s:Reason></s:Fault>'
                   % (code, text))

    def authorized(self, body):
        a = self.server.args
        user, pw = xml_text(body, "Username"), xml_text(body, "Password")
        nonce, created = xml_text(body, "Nonce"), xml_text(body, "Created")
        if not (user and pw and nonce and created):
            return False
        want = base64.b64encode(hashlib.sha1(base64.b64decode(nonce) + created.encode() +
                                             a.password.encode()).digest()).decode()
        return user == a.user and pw == want

    def do_POST(self):
        a = self.server.args
        body = self.rfile.read(int(self.headers.get("Content-Length", "0"))).decode(errors="replace")
        m = re.search(r"<s:Body>\s*<(?:\w+:)?(\w+)", body)
        op = m.group(1) if m else "?"
        base = "http://" + (a.advertise or "%s:%d" % (a.host, a.port))
        if op == "GetSystemDateAndTime":
            t = time.gmtime()
            return self.reply(200, "<tds:GetSystemDateAndTimeResponse><tds:SystemDateAndTime><tt:UTCDateTime>"
                                   "<tt:Time><tt:Hour>%d</tt:Hour><tt:Minute>%d</tt:Minute><tt:Second>%d"
                                   "</tt:Second></tt:Time><tt:Date><tt:Year>%d</tt:Year><tt:Month>%d</tt:Month>"
                                   "<tt:Day>%d</tt:Day></tt:Date></tt:UTCDateTime></tds:SystemDateAndTime>"
                                   "</tds:GetSystemDateAndTimeResponse>"
                              % (t.tm_hour, t.tm_min, t.tm_sec, t.tm_year, t.tm_mon, t.tm_mday))
        if not self.authorized(body):
            log("%s %s NOT AUTHORIZED" % (self.path, op))
            return self.fault("ter:NotAuthorized", "Sender not Authorized")
        log(self.path, op)
        if self.path == "/onvif/device_service" and op == "GetCapabilities":
            return self.reply(200, "<tds:GetCapabilitiesResponse><tds:Capabilities><tt:Media><tt:XAddr>"
                                   "%s/onvif/media_service</tt:XAddr></tt:Media></tds:Capabilities>"
                                   "</tds:GetCapabilitiesResponse>" % base)
        if self.path == "/onvif/media_service" and op == "GetProfiles":
            return self.reply(200, '<trt:GetProfilesResponse><trt:Profiles token="Profile_main" fixed="true">'
                                   '<tt:Name>main</tt:Name></trt:Profiles><trt:Profiles token="Profile_sub">'
                                   '<tt:Name>sub</tt:Name></trt:Profiles></trt:GetProfilesResponse>')
        if self.path == "/onvif/media_service" and op == "GetSnapshotUri":
            COUNTS["uri"] += 1
            if a.no_snapshot_uri:
                return self.fault("ter:ActionNotSupported", "no snapshots", status=500)
            if xml_text(body, "ProfileToken") != "Profile_main":
                return self.fault("ter:InvalidArgVal", "no such profile")
            return self.reply(200, "<trt:GetSnapshotUriResponse><trt:MediaUri><tt:Uri>%s/webcapture.jpg?"
                                   "command=snap&amp;channel=1</tt:Uri><tt:InvalidAfterConnect>false"
                                   "</tt:InvalidAfterConnect><tt:InvalidAfterReboot>false</tt:InvalidAfterReboot>"
                                   "<tt:Timeout>PT0S</tt:Timeout></trt:MediaUri></trt:GetSnapshotUriResponse>"
                              % base)
        return self.fault("ter:ActionNotSupported", "not here")

    def digest_ok(self):
        a = self.server.args
        h = self.headers.get("Authorization", "")
        if not h.startswith("Digest "):
            return False
        f = dict(re.findall(r'(\w+)="?([^",]*)"?', h[7:]))
        if f.get("username") != a.user or f.get("realm") != REALM or f.get("uri") != self.path:
            return False
        ha1 = md5("%s:%s:%s" % (a.user, REALM, a.password))
        ha2 = md5("GET:%s" % self.path)
        if f.get("qop") == "auth":
            want = md5("%s:%s:%s:%s:auth:%s" % (ha1, f.get("nonce"), f.get("nc"), f.get("cnonce"), ha2))
        else:
            want = md5("%s:%s:%s" % (ha1, f.get("nonce"), ha2))
        return f.get("response") == want and f.get("nonce") in self.server.nonces

    def do_GET(self):
        a = self.server.args
        path = self.path.split("?")[0]
        if path == "/webcapture.jpg":
            if "channel=1" not in self.path or "&amp;" in self.path:
                return self.send(400, "text/plain", b"bad query\n")
            if not self.digest_ok():
                nonce = secrets.token_hex(16)
                self.server.nonces.add(nonce)
                log("GET %s: 401 (Digest)" % self.path)
                return self.send(401, "text/plain", b"login\n", [
                    ("WWW-Authenticate", 'Digest realm="%s", qop="auth", nonce="%s", opaque="abc"' % (REALM, nonce))])
            COUNTS["digest"] += 1
            log("GET %s: the JPEG (Digest)" % self.path)
            return self.send(200, "image/jpeg", self.server.jpeg)
        if path == "/cgi-bin/snapshot.cgi":
            want = "Basic " + base64.b64encode(("%s:%s" % (a.user, a.password)).encode()).decode()
            if self.headers.get("Authorization") != want:
                log("GET %s: 401 (Basic)" % self.path)
                return self.send(401, "text/plain", b"login\n", [("WWW-Authenticate", 'Basic realm="%s"' % REALM)])
            COUNTS["basic"] += 1
            log("GET %s: the JPEG (Basic)" % self.path)
            return self.send(200, "image/jpeg", self.server.jpeg)
        return self.send(404, "text/plain", b"not found\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8899)
    ap.add_argument("--user", default="admin")
    ap.add_argument("--password", default="")
    ap.add_argument("--snapshot", default=os.path.join(HERE, "data", "snapshot_640x360.jpg"))
    ap.add_argument("--no-snapshot-uri", action="store_true")
    ap.add_argument("--advertise", default="")
    args = ap.parse_args()
    srv = Server((args.host, args.port), Handler)
    srv.args = args
    srv.nonces = set()
    srv.jpeg = open(args.snapshot, "rb").read()
    log("snapshots on http://%s:%d/onvif/device_service" % (args.host, args.port))
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    sys.exit(main())
