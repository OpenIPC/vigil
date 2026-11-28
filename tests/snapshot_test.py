#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""End-to-end test of snapshots (docs/api.md, Snapshots; docs/onvif.md).

    snapshot_test.py VIGILD CLIP

Starts fake cameras and vigild (generic platform: no decoder, so the
recorder's own picture is never there) with the API's logins on, then:
  - the camera's own JPEG through each way vigil asks for one: NETIP's
    snapshot in one message, split, framed as a media frame, refused; an
    RTSP camera's ONVIF GetSnapshotUri behind HTTP Digest; a snapshot_url
    behind Basic (fake_onvif_snap.py);
  - the API: GET .../snapshot.jpg with Basic and with Digest, the source
    chosen, a kept one listed, read, deleted; permissions;
  - the ONVIF server: SnapshotUri in the capabilities, GetSnapshotUri on
    media and media2 to the API's address, fetched with Digest;
  - a camera event (the API's injection): event_count pictures named after
    it, mailed with them attached to an SMTP sink here (when curl is there);
  - snapshot.max_mb: the oldest go.
Every JPEG is decoded with PIL when it is there. Only the standard
library otherwise.
"""
import base64
import email
import hashlib
import http.client
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
# openssl passwd -6 -salt vigiltestsalt secret
HASH = ("$6$vigiltestsalt$YNf0cVVTPqcmB27N7LaoJSiqJTsS7DvT6GejoILXrgfuBwdj8rCp9"
        "lnFrtSM5TO4kEOwYmcBLCsL4TzvT5U4b1")
MEDIA = "http://www.onvif.org/ver10/media/wsdl"
MEDIA2 = "http://www.onvif.org/ver20/media/wsdl"
JPEG = open(os.path.join(HERE, "data", "snapshot_640x360.jpg"), "rb").read()

try:
    from PIL import Image
    import io
except ImportError:
    Image = None

failures = []


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what, flush=True)
    if not cond:
        failures.append(what)
    return cond


def free_port(kind=socket.SOCK_STREAM):
    s = socket.socket(socket.AF_INET, kind)
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def wait_port(port, secs=10):
    end = time.time() + secs
    while time.time() < end:
        try:
            socket.create_connection(("127.0.0.1", port), timeout=0.5).close()
            return True
        except OSError:
            time.sleep(0.1)
    return False


def jpeg_size(b):
    """(w, h) of a JPEG PIL decodes (or that starts and ends as one without PIL); None otherwise."""
    if not b or b[:2] != b"\xff\xd8" or b[-2:] != b"\xff\xd9":
        return None
    if Image is None:
        return (-1, -1)
    try:
        im = Image.open(io.BytesIO(b))
        im.load()
        return im.size
    except Exception:
        return None


def req(port, method, path, auth=None, body=None, headers=None):
    c = http.client.HTTPConnection("127.0.0.1", port, timeout=30)
    h = dict(headers or {})
    if auth:
        h["Authorization"] = auth
    c.request(method, path, body, h)
    r = c.getresponse()
    out = r.status, r.headers, r.read()
    c.close()
    return out


def basic(user="root", password="secret"):
    return "Basic " + base64.b64encode(("%s:%s" % (user, password)).encode()).decode()


def digest_get(port, path, user="root", password="secret", nc=1, www=None, cn=None):
    """GET with HTTP Digest: the 401's Digest challenge (or `www`), then the answer."""
    if www is None:
        st, hdr, _ = req(port, "GET", path)
        www = [v for k, v in hdr.items() if k.lower() == "www-authenticate" and v.startswith("Digest ")]
        if st != 401 or not www:
            return st, hdr, b"", None
        www = www[0]
    nonce = re.search(r'nonce="([^"]+)"', www).group(1)
    realm = re.search(r'realm="([^"]+)"', www).group(1)
    ha1 = hashlib.md5(("%s:%s:%s" % (user, realm, password)).encode()).hexdigest()
    ha2 = hashlib.md5(("GET:%s" % path).encode()).hexdigest()
    cn = cn or os.urandom(8).hex()
    resp = hashlib.md5(("%s:%s:%08x:%s:auth:%s" % (ha1, nonce, nc, cn, ha2)).encode()).hexdigest()
    auth = ('Digest username="%s", realm="%s", nonce="%s", uri="%s", response="%s", qop=auth, nc=%08x, '
            'cnonce="%s", algorithm=MD5' % (user, realm, nonce, path, resp, nc, cn))
    st, hdr, b = req(port, "GET", path, auth)
    return st, hdr, b, www


def wsse(user, password):
    nonce = os.urandom(16)
    created = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    d = base64.b64encode(hashlib.sha1(nonce + created.encode() + password.encode()).digest()).decode()
    return ('<s:Header><Security xmlns="http://docs.oasis-open.org/wss/2004/01/'
            'oasis-200401-wss-wssecurity-secext-1.0.xsd"><UsernameToken><Username>%s</Username>'
            '<Password Type="http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-username-token-profile-1.0'
            '#PasswordDigest">%s</Password><Nonce>%s</Nonce><Created xmlns="http://docs.oasis-open.org/wss/2004/01/'
            'oasis-200401-wss-wssecurity-utility-1.0.xsd">%s</Created></UsernameToken></Security></s:Header>'
            % (user, d, base64.b64encode(nonce).decode(), created))


def soap(port, path, ns, op, args="", user="root", password="secret"):
    env = ('<?xml version="1.0" encoding="UTF-8"?><s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope">'
           '%s<s:Body><%s xmlns="%s">%s</%s></s:Body></s:Envelope>' % (wsse(user, password), op, ns, args, op))
    st, _, b = req(port, "POST", path, None, env.encode(), {"Content-Type": "application/soap+xml"})
    return st, b.decode("utf-8", "replace")


class Smtp(threading.Thread):
    """An SMTP sink: every message's DATA kept."""

    def __init__(self):
        super().__init__(daemon=True)
        self.srv = socket.socket()
        self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.srv.bind(("127.0.0.1", 0))
        self.srv.listen(4)
        self.port = self.srv.getsockname()[1]
        self.messages = []

    def run(self):
        while True:
            try:
                c, _ = self.srv.accept()
            except OSError:
                return
            threading.Thread(target=self.session, args=(c,), daemon=True).start()

    def session(self, c):
        f = c.makefile("rb")
        c.sendall(b"220 sink ESMTP\r\n")
        try:
            while True:
                line = f.readline()
                if not line:
                    return
                cmd = line.strip().upper()
                if cmd.startswith(b"EHLO"):
                    c.sendall(b"250-sink\r\n250 8BITMIME\r\n")
                elif cmd.startswith(b"DATA"):
                    c.sendall(b"354 go on\r\n")
                    data = b""
                    while True:
                        l2 = f.readline()
                        if not l2 or l2 == b".\r\n":
                            break
                        data += l2[1:] if l2.startswith(b"..") else l2
                    self.messages.append(data)
                    c.sendall(b"250 kept\r\n")
                elif cmd.startswith(b"QUIT"):
                    c.sendall(b"221 bye\r\n")
                    return
                else:
                    c.sendall(b"250 ok\r\n")
        finally:
            c.close()


def main():
    vigild, clip = sys.argv[1], sys.argv[2]
    tmp = tempfile.mkdtemp(prefix="vigil-snap-", dir=os.getcwd())
    api_port, onvif_port, snap_port = free_port(), free_port(), free_port()
    wsd_port = free_port(socket.SOCK_DGRAM)
    modes = ["one", "split", "framed", "refuse"]
    netip_ports = [free_port() for _ in modes]
    smtp = Smtp()
    smtp.start()
    rec = os.path.join(tmp, "rec", "cams")
    snapdir = os.path.join(tmp, "rec", "snapshots")
    shadow = os.path.join(tmp, "shadow")
    with open(shadow, "w") as f:
        f.write("root:%s:19000:0:99999:7:::\n" % HASH)
    cams = ""
    for i, p in enumerate(netip_ports):
        cams += '  - url: "netip://admin:cam@127.0.0.1:%d/"\n' % p
        if i == 0:
            cams += '    name: Gate\n    snapshot_on_event: "on"\n'
        # Its ONVIF (asked when NETIP has no snapshot) where nothing listens:
        # not whatever else on this host answers on 80 or 8899.
        cams += '    onvif_url: "http://127.0.0.1:%d/onvif/device_service"\n' % free_port()
    cams += ('  - url: "rtsp://admin:cam@127.0.0.1:1/onvif"\n'
             '    onvif_url: "http://127.0.0.1:%d/onvif/device_service"\n'
             '  - url: "rtsp://admin:cam@127.0.0.1:1/plain"\n'
             '    snapshot_url: "http://127.0.0.1:%d/cgi-bin/snapshot.cgi"\n' % (snap_port, snap_port))
    cfg = os.path.join(tmp, "vigil.yaml")
    with open(cfg, "w") as f:
        f.write("cameras:\n%s"
                "record:\n  path: %s\n  allow_rootfs: true\n"
                "snapshot:\n  event_count: 2\n  event_interval: 1\n  mail: true\n  max_mb: 1\n"
                "smtp:\n  enabled: true\n  server: 127.0.0.1\n  port: %d\n  security: none\n"
                "  from: nvr@example.org\n  to: owner@example.org\n"
                "api:\n  listen: 127.0.0.1:%d\n  shadow: %s\n  users: %s\n"
                "onvif_server:\n  enabled: true\n  port: %d\n"
                "netip_server:\n  enabled: false\nrtsp_server:\n  enabled: false\n"
                % (cams, json.dumps(rec), smtp.port, api_port, json.dumps(shadow),
                   json.dumps(os.path.join(tmp, "users.yaml")), onvif_port))
    procs = []
    for mode, p in zip(modes, netip_ports):
        procs.append(subprocess.Popen([sys.executable, os.path.join(HERE, "netip_server.py"), clip, "--port", str(p),
                                       "--password", "cam", "--snap-mode", mode],
                                      stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT))
    snap_log = open(os.path.join(tmp, "fake_snap.log"), "w")
    procs.append(subprocess.Popen([sys.executable, os.path.join(HERE, "fake_onvif_snap.py"), "--port", str(snap_port),
                                   "--password", "cam"], stdout=snap_log, stderr=subprocess.STDOUT))
    for p in netip_ports + [snap_port]:
        wait_port(p)
    log_path = os.path.join(tmp, "vigild.log")
    env = dict(os.environ, VIGIL_ONVIF_WSD_PORT=str(wsd_port))
    vd = subprocess.Popen([vigild, "-f", cfg, "-t", "120"], stdout=open(log_path, "w"), stderr=subprocess.STDOUT,
                          env=env)
    try:
        end = time.time() + 10
        while time.time() < end and "onvif: device service" not in open(log_path).read():
            time.sleep(0.2)
        if not check("onvif: device service" in open(log_path).read(), "vigild up"):
            return
        st, _, _ = req(api_port, "POST", "/api/v1/users", basic(),
                        json.dumps({"name": "anna", "group": "user", "password": "annapw"}).encode())
        check(st in (200, 201), "user anna (live, playback)")
        st, _, _ = req(api_port, "POST", "/api/v1/groups", basic(),
                        json.dumps({"name": "lookers", "permissions": {"live": True}}).encode())
        st2, _, _ = req(api_port, "POST", "/api/v1/users", basic(),
                         json.dumps({"name": "olga", "group": "lookers", "password": "olgapw"}).encode())
        check(st in (200, 201) and st2 in (200, 201), "user olga (live only)")

        # ---- the camera's own JPEG, each way
        want = {1: "camera/netip", 2: "camera/netip", 3: "camera/netip", 5: "camera/onvif", 6: "camera/url"}
        for ch, src in sorted(want.items()):
            st, hdr, b = req(api_port, "GET", "/api/v1/channels/%d/snapshot.jpg" % ch, basic())
            check(st == 200 and hdr.get("Content-Type") == "image/jpeg" and hdr.get("X-Vigil-Source") == src and
                  b == JPEG and jpeg_size(b) in ((640, 360), (-1, -1)),
                  "CAM%d: the camera's JPEG via %s (%s, %s, %d bytes)" % (ch, src, st, hdr.get("X-Vigil-Source"),
                                                                       len(b)))
        st, _, b = req(api_port, "GET", "/api/v1/channels/4/snapshot.jpg?source=camera", basic())
        check(st == 503 and b"Ret 103" in b, "CAM4 refuses its NETIP snapshot: 503 with the reason (%s %s)" % (st, b[:120]))
        st, _, b = req(api_port, "GET", "/api/v1/channels/4/snapshot.jpg", basic())
        check(st == 503 and b"not being decoded" in b, "CAM4, auto: no decoder picture here either: 503")
        st, _, b = req(api_port, "GET", "/api/v1/channels/1/snapshot.jpg?source=nvr", basic())
        check(st == 503, "source=nvr on a platform without a decoder: 503")
        st, _, _ = req(api_port, "GET", "/api/v1/channels/1/snapshot.jpg?source=other", basic())
        check(st == 400, "an unknown source: 400")
        st, _, _ = req(api_port, "GET", "/api/v1/channels/9/snapshot.jpg", basic())
        check(st == 404, "no camera on channel 9: 404")
        st, _, b = req(api_port, "HEAD", "/api/v1/channels/1/snapshot.jpg", basic())
        check(st == 200 and not b, "HEAD: 200 without the picture")

        # ---- logins: none, Basic, Digest (with its nonce count), permissions
        st, hdr, _ = req(api_port, "GET", "/api/v1/channels/1/snapshot.jpg")
        www = [v for k, v in hdr.items() if k.lower() == "www-authenticate"]
        check(st == 401 and len(www) == 2 and www[0].startswith("Basic ") and www[1].startswith("Digest ") and
              'qop="auth"' in www[1], "no login: 401 offering Basic and Digest (%s)" % www)
        st, _, b, chal = digest_get(api_port, "/api/v1/channels/1/snapshot.jpg")
        check(st == 200 and b == JPEG, "Digest: the JPEG (%s)" % st)
        st, _, b, _ = digest_get(api_port, "/api/v1/channels/1/snapshot.jpg", nc=2, www=chal, cn="c0ffee")
        check(st == 200 and b == JPEG, "Digest: the same nonce, the next count")
        st, hdr, _, _ = digest_get(api_port, "/api/v1/channels/1/snapshot.jpg", nc=2, www=chal, cn="c0ffee")
        check(st == 401 and "stale=true" in str(hdr), "Digest: the same request again (a replay): 401, stale")
        st, _, b, _ = digest_get(api_port, "/api/v1/channels/2/snapshot.jpg?source=camera")
        check(st == 200 and b == JPEG, "Digest with a query")
        st, _, _, _ = digest_get(api_port, "/api/v1/channels/1/snapshot.jpg", password="wrong")
        check(st == 401, "Digest, a wrong password: 401")
        st, _, b, _ = digest_get(api_port, "/api/v1/channels/1/snapshot.jpg", "olga", "olgapw")
        check(st == 200, "live permission: a snapshot (%s)" % st)
        st, _, _ = req(api_port, "GET", "/api/v1/snapshots", basic("olga", "olgapw"))
        check(st == 403, "live permission alone: the kept ones are not listed (%s)" % st)

        # ---- kept: POST, list, read, delete
        st, _, b = req(api_port, "POST", "/api/v1/channels/6/snapshot", basic("anna", "annapw"))
        doc = json.loads(b) if st == 201 else {}
        name = doc.get("file", "")
        check(st == 201 and re.match(r"ch06_\d{8}-\d{6}-\d{3}_manual\.jpg$", name) and doc.get("source") == "camera/url",
              "POST: kept as %s" % name)
        check(os.path.isfile(os.path.join(snapdir, name)), "in the directory next to the recordings")
        st, _, b = req(api_port, "GET", "/api/v1/snapshots?channel=6", basic("anna", "annapw"))
        lst = json.loads(b) if st == 200 else {}
        check(st == 200 and lst.get("dir") == snapdir and [s["file"] for s in lst.get("snapshots", [])] == [name] and
              lst["snapshots"][0]["kind"] == "manual" and lst["snapshots"][0]["size"] == len(JPEG),
              "listed, by channel")
        st, hdr, b = req(api_port, "GET", "/api/v1/snapshots/" + name, basic("anna", "annapw"))
        check(st == 200 and b == JPEG and name in hdr.get("Content-Disposition", ""), "read back")
        st, _, _ = req(api_port, "GET", "/api/v1/snapshots/..%2Fvigil.yaml", basic())
        st2, _, _ = req(api_port, "GET", "/api/v1/snapshots/vigil.yaml", basic())
        check(st == 404 and st2 == 404, "only snapshots' names are served (%s %s)" % (st, st2))
        st, _, _ = req(api_port, "DELETE", "/api/v1/snapshots/" + name, basic("anna", "annapw"))
        check(st == 403, "deleting needs the storage permission (%s)" % st)
        st, _, _ = req(api_port, "DELETE", "/api/v1/snapshots/" + name, basic())
        st2, _, _ = req(api_port, "GET", "/api/v1/snapshots/" + name, basic())
        check(st == 200 and st2 == 404 and not os.path.exists(os.path.join(snapdir, name)), "deleted")

        # ---- ONVIF: GetSnapshotUri to the API, fetched with Digest
        st, b = soap(onvif_port, "/onvif/media_service", MEDIA, "GetServiceCapabilities")
        check(st == 200 and 'SnapshotUri="true"' in b, "media: SnapshotUri=\"true\"")
        st, b = soap(onvif_port, "/onvif/media2_service", MEDIA2, "GetServiceCapabilities")
        check(st == 200 and 'SnapshotUri="true"' in b, "media2: SnapshotUri=\"true\"")
        st, b = soap(onvif_port, "/onvif/media_service", MEDIA, "GetSnapshotUri",
                     "<ProfileToken>ch1_main</ProfileToken>")
        uri = re.search(r"<tt:Uri>([^<]+)</tt:Uri>", b)
        uri = uri.group(1) if uri else ""
        check(st == 200 and uri == "http://127.0.0.1:%d/api/v1/channels/1/snapshot.jpg" % api_port,
              "media GetSnapshotUri: the API's address (%s)" % uri)
        st, b = soap(onvif_port, "/onvif/media2_service", MEDIA2, "GetSnapshotUri",
                     "<ProfileToken>ch5_main</ProfileToken>")
        uri2 = re.search(r"<tr2:Uri>([^<]+)</tr2:Uri>", b)
        check(st == 200 and uri2 and uri2.group(1).endswith("/api/v1/channels/5/snapshot.jpg"),
              "media2 GetSnapshotUri (%s)" % (uri2.group(1) if uri2 else b[:200]))
        st, b = soap(onvif_port, "/onvif/media_service", MEDIA, "GetSnapshotUri", "<ProfileToken>ch99_main</ProfileToken>")
        check(st == 400 and "NoProfile" in b, "an unknown profile: NoProfile")
        path = uri[uri.index("/api"):] if "/api" in uri else "/"
        st, _, b, _ = digest_get(api_port, path, "anna", "annapw")
        check(st == 200 and jpeg_size(b) in ((640, 360), (-1, -1)), "its URI fetched with Digest: a JPEG")

        # ---- on an event: event_count pictures, named after it, mailed
        before = set(os.listdir(snapdir)) if os.path.isdir(snapdir) else set()
        st, _, _ = req(api_port, "POST", "/api/v1/channels/1/events", basic(),
                        json.dumps({"events": ["motion", "human"]}).encode())
        check(st in (200, 202), "motion and a person on CAM1 (%s)" % st)
        new = []
        end = time.time() + 10
        while time.time() < end:
            new = sorted(set(os.listdir(snapdir)) - before)
            if len(new) >= 2:
                break
            time.sleep(0.2)
        time.sleep(1.5)
        new = sorted(n for n in set(os.listdir(snapdir)) - before if not n.startswith("."))
        check(len(new) == 2 and all(re.match(r"ch01_.*_motion\.jpg$", n) for n in new),
              "two pictures (event_count), named after the event, one burst for both types: %s" % new)
        if new:
            with open(os.path.join(snapdir, new[0]), "rb") as f:
                check(jpeg_size(f.read()) in ((640, 360), (-1, -1)), "and they open")
        st, _, _ = req(api_port, "POST", "/api/v1/channels/2/events", basic(), json.dumps({"events": ["motion"]}).encode())
        time.sleep(2)
        check(not [n for n in os.listdir(snapdir) if n.startswith("ch02_")], "CAM2 (snapshot_on_event off): none")
        if shutil.which("curl"):
            # (mgmt mails events of its own to the same address: the one with pictures)
            def ours():
                for m in list(smtp.messages):
                    p = email.message_from_bytes(m)
                    if "motion" in p.get("Subject", ""):
                        return p
                return None
            end = time.time() + 15
            while time.time() < end and not ours():
                time.sleep(0.2)
            msg = ours()
            atts = [p for p in msg.walk() if p.get_content_type() == "image/jpeg"] if msg else []
            check(msg is not None and "CAM1 (Gate): motion" in msg.get("Subject", "") and len(atts) == 2 and
                  all(p.get_payload(decode=True) == JPEG for p in atts) and
                  sorted(p.get_filename() for p in atts) == new,
                  "mailed: the subject, both pictures attached (%s, %d)" %
                  (msg.get("Subject") if msg else None, len(atts)))
        else:
            print("skip mail: no curl here")

        # ---- the cap: the oldest go
        os.makedirs(snapdir, exist_ok=True)
        old = ["ch03_20200101-000000-000_manual.jpg", "ch03_20200101-000001-000_manual.jpg"]
        for n in old:
            with open(os.path.join(snapdir, n), "wb") as f:
                f.write(b"\xff\xd8" + b"\0" * (600 << 10) + b"\xff\xd9")
        st, _, _ = req(api_port, "POST", "/api/v1/channels/3/snapshot", basic())
        left = os.listdir(snapdir)
        check(st == 201 and old[0] not in left and old[1] in left,
              "max_mb 1: the oldest went to make room, the next one stayed")
    finally:
        vd.terminate()
        try:
            vd.wait(10)
        except subprocess.TimeoutExpired:
            vd.kill()
        for p in procs:
            p.terminate()
            p.wait()
        smtp.srv.close()
        if failures:
            print("--- vigild log ---")
            print(open(log_path).read()[-4000:])
            print("--- fake ONVIF snapshots ---")
            print(open(os.path.join(tmp, "fake_snap.log")).read()[-2000:])
    if not failures:
        shutil.rmtree(tmp, ignore_errors=True)
    print("%d failed" % len(failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
