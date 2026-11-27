#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""End-to-end test of vigild's ONVIF server (docs/onvif.md).

    onvif_test.py VIGILD CLIP

Starts the NETIP camera emulator with CLIP and vigild (generic platform)
with the ONVIF server on a free port and WS-Discovery on a free UDP port
(VIGIL_ONVIF_WSD_PORT), creates users through the API, then plays an
ONVIF client: a WS-Discovery probe, the operations allowed before a
login, GetProfiles / GetStreamUri on media and media2 with a
WS-UsernameToken digest and with HTTP Digest, replays and wrong
passwords refused, permissions, PasswordText for the system account.
Only the standard library.
"""
import base64
import shutil
import hashlib
import http.client
import json
import os
import re
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request
import uuid

HERE = os.path.dirname(os.path.abspath(__file__))
# openssl passwd -6 -salt vigiltestsalt secret
HASH = ("$6$vigiltestsalt$YNf0cVVTPqcmB27N7LaoJSiqJTsS7DvT6GejoILXrgfuBwdj8rCp9"
        "lnFrtSM5TO4kEOwYmcBLCsL4TzvT5U4b1")
DEV = "http://www.onvif.org/ver10/device/wsdl"
MEDIA = "http://www.onvif.org/ver10/media/wsdl"
MEDIA2 = "http://www.onvif.org/ver20/media/wsdl"

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


def wsse(user, password, nonce=None, created=None):
    nonce = nonce or os.urandom(16)
    created = created or time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    digest = base64.b64encode(hashlib.sha1(nonce + created.encode() + password.encode()).digest()).decode()
    return ('<s:Header><Security xmlns="http://docs.oasis-open.org/wss/2004/01/'
            'oasis-200401-wss-wssecurity-secext-1.0.xsd"><UsernameToken><Username>%s</Username>'
            '<Password Type="http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-username-token-profile-1.0'
            '#PasswordDigest">%s</Password><Nonce>%s</Nonce><Created xmlns="http://docs.oasis-open.org/wss/2004/01/'
            'oasis-200401-wss-wssecurity-utility-1.0.xsd">%s</Created></UsernameToken></Security></s:Header>'
            % (user, digest, base64.b64encode(nonce).decode(), created))


def text_token(user, password):
    return ('<s:Header><Security><UsernameToken><Username>%s</Username><Password>%s</Password>'
            '</UsernameToken></Security></s:Header>' % (user, password))


def envelope(ns, op, args="", header=""):
    return ('<?xml version="1.0" encoding="UTF-8"?><s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope">'
            '%s<s:Body><%s xmlns="%s">%s</%s></s:Body></s:Envelope>' % (header, op, ns, args, op))


class Onvif:
    def __init__(self, port):
        self.port = port

    def post(self, path, body, headers=None):
        c = http.client.HTTPConnection("127.0.0.1", self.port, timeout=15)
        h = {"Content-Type": "application/soap+xml; charset=utf-8"}
        h.update(headers or {})
        c.request("POST", path, body.encode(), h)
        r = c.getresponse()
        out = r.status, dict(r.getheaders()), r.read().decode("utf-8", "replace")
        c.close()
        return out

    def call(self, ns, op, args="", header="", path=None):
        path = path or ("/onvif/media2_service" if ns == MEDIA2 else
                        "/onvif/media_service" if ns == MEDIA else "/onvif/device_service")
        return self.post(path, envelope(ns, op, args, header))

    def digest(self, ns, op, user, password, args=""):
        """HTTP Digest: the challenge, then the answer to it."""
        path = "/onvif/media_service" if ns == MEDIA else "/onvif/device_service"
        st, hdr, _ = self.call(ns, op, args)
        www = hdr.get("WWW-Authenticate", "")
        nonce = re.search(r'nonce="([^"]+)"', www)
        realm = re.search(r'realm="([^"]+)"', www)
        if st != 401 or not nonce:
            return st, hdr, ""
        ha1 = hashlib.md5(("%s:%s:%s" % (user, realm.group(1), password)).encode()).hexdigest()
        ha2 = hashlib.md5(("POST:%s" % path).encode()).hexdigest()
        cn = os.urandom(8).hex()
        resp = hashlib.md5(("%s:%s:00000001:%s:auth:%s" % (ha1, nonce.group(1), cn, ha2)).encode()).hexdigest()
        auth = ('Digest username="%s", realm="%s", nonce="%s", uri="%s", response="%s", qop=auth, nc=00000001, '
                'cnonce="%s", algorithm=MD5' % (user, realm.group(1), nonce.group(1), path, resp, cn))
        return self.post(path, envelope(ns, op, args), {"Authorization": auth})


def api(port, method, path, body=None):
    req = urllib.request.Request("http://127.0.0.1:%d%s" % (port, path), method=method,
                                 data=None if body is None else json.dumps(body).encode())
    req.add_header("Authorization", "Basic " + base64.b64encode(b"root:secret").decode())
    req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status
    except urllib.error.HTTPError as e:
        return e.code


def probe(port, types="dn:NetworkVideoTransmitter", wait=2.0):
    """A WS-Discovery Probe to 127.0.0.1; the answers (texts) within `wait`."""
    msgid = "uuid:%s" % uuid.uuid4()
    msg = ('<?xml version="1.0" encoding="UTF-8"?><s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope" '
           'xmlns:a="http://schemas.xmlsoap.org/ws/2004/08/addressing" '
           'xmlns:d="http://schemas.xmlsoap.org/ws/2005/04/discovery" '
           'xmlns:dn="http://www.onvif.org/ver10/network/wsdl"><s:Header><a:MessageID>%s</a:MessageID>'
           '<a:To>urn:schemas-xmlsoap-org:ws:2005:04:discovery</a:To>'
           '<a:Action>http://schemas.xmlsoap.org/ws/2005/04/discovery/Probe</a:Action></s:Header>'
           '<s:Body><d:Probe><d:Types>%s</d:Types></d:Probe></s:Body></s:Envelope>' % (msgid, types))
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(0.2)
    s.sendto(msg.encode(), ("127.0.0.1", port))
    out, end = [], time.time() + wait
    while time.time() < end:
        try:
            out.append(s.recv(65536).decode())
        except socket.timeout:
            pass
    s.close()
    return msgid, out


def main():
    vigild, clip = sys.argv[1], sys.argv[2]
    tmp = tempfile.mkdtemp(prefix="vigil-onvif-", dir=os.getcwd())
    api_port, cam_port, onvif_port, rtsp_port = free_port(), free_port(), free_port(), free_port()
    wsd_port = free_port(socket.SOCK_DGRAM)
    shadow = os.path.join(tmp, "shadow")
    with open(shadow, "w") as f:
        f.write("root:%s:19000:0:99999:7:::\n" % HASH)
    cfg = os.path.join(tmp, "vigil.yaml")
    with open(cfg, "w") as f:
        f.write("cameras:\n  - url: \"netip://admin:cam@127.0.0.1:%d/\"\n    name: \"Gate <1>\"\n"
                "api:\n  listen: 127.0.0.1:%d\n  shadow: %s\n  users: %s\n"
                "onvif_server:\n  enabled: true\n  port: %d\n"
                "rtsp_server:\n  port: %d\n"
                % (cam_port, api_port, json.dumps(shadow), json.dumps(os.path.join(tmp, "users.yaml")), onvif_port,
                   rtsp_port))
    cam = subprocess.Popen([sys.executable, os.path.join(HERE, "netip_server.py"), clip, "--port", str(cam_port),
                            "--password", "cam"], stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    log_path = os.path.join(tmp, "vigild.log")
    env = dict(os.environ, VIGIL_ONVIF_WSD_PORT=str(wsd_port))
    vd = subprocess.Popen([vigild, "-f", cfg, "-t", "90"], stdout=open(log_path, "w"), stderr=subprocess.STDOUT,
                          env=env)
    o = Onvif(onvif_port)
    try:
        end = time.time() + 10
        while time.time() < end and "onvif: device service" not in open(log_path).read():
            time.sleep(0.2)
        if not check("onvif: device service" in open(log_path).read(), "vigild serves ONVIF"):
            return
        check(api(api_port, "POST", "/api/v1/users", {"name": "anna", "group": "user", "password": "annapw"}) in (200, 201),
              "user anna (live, playback)")
        check(api(api_port, "POST", "/api/v1/groups", {"name": "replay", "permissions": {"playback": True}}) in (200, 201),
              "group without live view")
        check(api(api_port, "POST", "/api/v1/users", {"name": "bob", "group": "replay", "password": "bobpw1"}) in (200, 201),
              "user bob in it")

        # ---- WS-Discovery
        msgid, ans = probe(wsd_port)
        m = ans[0] if ans else ""
        check(len(ans) == 1 and "ProbeMatches" in m and "<a:RelatesTo>%s</a:RelatesTo>" % msgid in m and
              "<d:XAddrs>http://127.0.0.1:%d/onvif/device_service</d:XAddrs>" % onvif_port in m and
              "onvif://www.onvif.org/Profile/Streaming" in m, "a probe is answered with our XAddrs")
        _, ans = probe(wsd_port, types="dn:NetworkVideoDisplay", wait=1.0)
        check(not ans, "a probe for something else is not")

        # ---- before logging in
        st, _, b = o.call(DEV, "GetSystemDateAndTime")
        check(st == 200 and "<tt:UTCDateTime>" in b and "<tt:Year>%d</tt:Year>" % time.gmtime().tm_year in b,
              "GetSystemDateAndTime without a login (%s)" % st)
        st, _, b = o.call(DEV, "GetCapabilities", "<Category>All</Category>")
        check(st == 200 and "<tt:XAddr>http://127.0.0.1:%d/onvif/media_service</tt:XAddr>" % onvif_port in b,
              "GetCapabilities without a login")
        st, _, b = o.call(DEV, "GetServices", "<IncludeCapability>true</IncludeCapability>")
        check(st == 200 and MEDIA2 in b and "/onvif/media2_service" in b and "tr2:Capabilities" in b,
              "GetServices lists media2")
        st, hdr, b = o.call(MEDIA, "GetProfiles")
        check(st == 401 and hdr.get("WWW-Authenticate", "").startswith("Digest ") and "ter:NotAuthorized" in b,
              "GetProfiles without a login: 401, Digest asked for (%s)" % st)
        st, _, b = o.call(DEV, "GetDeviceInformation")
        check(st == 401, "GetDeviceInformation without a login (%s)" % st)

        # ---- WS-UsernameToken
        st, _, b = o.call(DEV, "GetDeviceInformation", header=wsse("anna", "annapw"))
        check(st == 200 and "<tds:Manufacturer>OpenIPC</tds:Manufacturer>" in b and "<tds:Model>GENERIC NVR" in b,
              "GetDeviceInformation with a digest token (%s)" % st)
        # The camera's size arrives with its first keyframe.
        b, end = "", time.time() + 15
        while time.time() < end and "<tt:Width>640</tt:Width>" not in b:
            st, _, b = o.call(MEDIA, "GetProfiles", header=wsse("anna", "annapw"))
            time.sleep(0.5)
        check(st == 200 and 'token="ch1_main"' in b and 'token="ch1_sub"' in b and "<tt:Width>640</tt:Width>" in b
              and "<tt:Name>Gate &lt;1&gt; main</tt:Name>" in b, "media GetProfiles: ch1_main, ch1_sub, 640x360")
        st, _, b = o.call(MEDIA, "GetStreamUri", "<StreamSetup><Stream>RTP-Unicast</Stream></StreamSetup>"
                          "<ProfileToken>ch1_sub</ProfileToken>", header=wsse("anna", "annapw"))
        m = re.search(r"<tt:Uri>([^<]*)</tt:Uri>", b)
        uri = m.group(1) if m else ""
        check(st == 200 and uri == "rtsp://127.0.0.1:%d/ch1/sub" % rtsp_port,
              "media GetStreamUri ch1_sub: rtsp_server.port")
        if shutil.which("ffprobe"):
            # The URI plays, with the same user (its RTSP digest set with its password).
            r = subprocess.run(["ffprobe", "-v", "error", "-rtsp_transport", "tcp", "-show_entries",
                                "stream=codec_name,width,height", "-of", "csv=p=0",
                                uri.replace("rtsp://", "rtsp://anna:annapw@")],
                               capture_output=True, text=True, timeout=30)
            check(r.stdout.strip() == "h264,640,360", "the URI plays: %s" % (r.stdout.strip() or r.stderr[-200:]))
        st, _, b = o.call(MEDIA, "GetStreamUri", "<ProfileToken>ch7_main</ProfileToken>", header=wsse("anna", "annapw"))
        check(st == 400 and "ter:NoProfile" in b, "a profile that is not there (%s)" % st)
        st, _, b = o.call(MEDIA2, "GetProfiles", "<Type>All</Type>", header=wsse("anna", "annapw"))
        check(st == 200 and "<tr2:VideoEncoder" in b and "<tt:Encoding>H264</tt:Encoding>" in b,
              "media2 GetProfiles with configurations")
        st, _, b = o.call(MEDIA2, "GetStreamUri", "<Protocol>RtspUnicast</Protocol><ProfileToken>ch1_main"
                          "</ProfileToken>", header=wsse("anna", "annapw"))
        check(st == 200 and "<tr2:Uri>rtsp://127.0.0.1:%d/ch1/main</tr2:Uri>" % rtsp_port in b,
              "media2 GetStreamUri")
        st, _, b = o.call(DEV, "GetNetworkProtocols", header=wsse("anna", "annapw"))
        check(st == 200 and "<tt:Name>RTSP</tt:Name><tt:Enabled>true</tt:Enabled><tt:Port>%d</tt:Port>" % rtsp_port in b,
              "GetNetworkProtocols: RTSP at rtsp_server.port")
        # The RTSP server off: no URI to give.
        check(api(api_port, "PUT", "/api/v1/config/rtsp_server", {"enabled": False, "port": rtsp_port}) == 200, "RTSP server off")
        st, _, b = o.call(MEDIA2, "GetStreamUri", "<Protocol>RtspUnicast</Protocol><ProfileToken>ch1_main"
                          "</ProfileToken>", header=wsse("anna", "annapw"))
        check(st == 500 and "ter:ActionNotSupported" in b and "rtsp_server.enabled" in b,
              "GetStreamUri with the RTSP server off: a fault (%s)" % st)
        st, _, b = o.call(DEV, "GetNetworkProtocols", header=wsse("anna", "annapw"))
        check("<tt:Name>RTSP</tt:Name><tt:Enabled>false</tt:Enabled>" in b, "GetNetworkProtocols: RTSP off")
        check(api(api_port, "PUT", "/api/v1/config/rtsp_server", {"enabled": True, "port": rtsp_port}) == 200, "RTSP server on again")
        st, _, b = o.call(MEDIA, "GetProfiles", path="/onvif/device_service",
                          header=wsse("anna", "annapw"))
        check(st == 200 and "trt:GetProfilesResponse" in b, "an operation goes by its namespace, not the path")

        nonce = os.urandom(16)
        h = wsse("anna", "annapw", nonce=nonce)
        st, _, _ = o.call(DEV, "GetScopes", header=h)
        st2, _, b = o.call(DEV, "GetScopes", header=h)
        check(st == 200 and st2 == 400 and "ter:NotAuthorized" in b, "a token used twice is refused (%s, %s)" % (st, st2))
        st, _, b = o.call(DEV, "GetScopes", header=wsse("anna", "annapw", created="2001-01-01T00:00:00Z"))
        check(st == 400, "a token made long ago is refused (%s)" % st)
        t0 = time.time()
        st, _, b = o.call(DEV, "GetScopes", header=wsse("anna", "wrong"))
        check(st == 400 and "ter:NotAuthorized" in b and time.time() - t0 >= 0.9,
              "a wrong password: refused, a second late (%s)" % st)

        # ---- HTTP Digest
        st, _, b = o.digest(MEDIA, "GetProfiles", "anna", "annapw")
        check(st == 200 and 'token="ch1_main"' in b, "GetProfiles with HTTP Digest (%s)" % st)
        st, _, _ = o.digest(MEDIA, "GetProfiles", "anna", "nope")
        check(st == 401, "HTTP Digest, wrong password (%s)" % st)

        # ---- permissions
        st, _, b = o.call(MEDIA, "GetProfiles", header=wsse("bob", "bobpw1"))
        check(st == 400 and "live permission" in b, "no live permission, no profiles (%s)" % st)
        st, _, b = o.call(DEV, "GetUsers", header=wsse("anna", "annapw"))
        check(st == 400 and "users permission" in b, "GetUsers needs the users permission")
        st, _, b = o.call(DEV, "SystemReboot", header=wsse("anna", "annapw"))
        check(st == 400 and "system permission" in b, "SystemReboot needs the system permission")

        # ---- the system account: no ONVIF key, so PasswordText only
        st, _, b = o.call(DEV, "GetUsers", header=wsse("root", "secret"))
        check(st == 400, "root with a digest token: refused (%s)" % st)
        st, _, b = o.call(DEV, "GetUsers", header=text_token("root", "secret"))
        check(st == 200 and "<tt:Username>anna</tt:Username><tt:UserLevel>User</tt:UserLevel>" in b and
              "<tt:Username>root</tt:Username><tt:UserLevel>Administrator</tt:UserLevel>" in b,
              "GetUsers as root with PasswordText (%s)" % st)
        st, _, b = o.call(DEV, "SystemReboot", header=text_token("root", "secret"))
        check(st == 500 and "does not reboot" in b, "SystemReboot: not on a system vigild does not manage")

        # ---- not SOAP, not supported
        st, _, b = o.post("/onvif/device_service", "hello")
        check(st == 400 and "ter:WellFormed" in b, "not SOAP: a fault (%s)" % st)
        st, _, b = o.call(DEV, "SetHostname", "<Name>x</Name>", header=wsse("anna", "annapw"))
        check(st == 500 and "ter:ActionNotSupported" in b, "an operation we do not have (%s)" % st)
        c = http.client.HTTPConnection("127.0.0.1", onvif_port, timeout=5)
        c.request("GET", "/onvif/device_service")
        check(c.getresponse().status == 405, "GET: 405")
        c.close()

        log = open(log_path).read()
        check("annapw" not in log and "secret" not in log, "no password in the log")
    finally:
        vd.terminate()
        try:
            vd.wait(10)
        except subprocess.TimeoutExpired:
            vd.kill()
        cam.terminate()
        cam.wait()
        if failures:
            print("vigild log:\n" + open(log_path).read()[-3000:])
    print("%d failed" % len(failures))
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
