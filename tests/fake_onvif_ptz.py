#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""A fake ONVIF PTZ camera, for vigil's PTZ tests and demos.

    fake_onvif_ptz.py [--host 127.0.0.1] [--port 8899] [--user admin]
                      [--password ''] [--log FILE] [--no-ptz] [--no-timeout]
                      [--tours N]

Answers SOAP 1.2 over HTTP with a checked WS-UsernameToken (as
fake_onvif_events.py does):
  /onvif/device_service   GetSystemDateAndTime (no login), GetCapabilities
                          (Media, PTZ, Imaging), GetServices
  /onvif/media_service    GetProfiles: Profile_1 (main) and Profile_2 (sub),
                          each with VideoSource_1 and PTZ node PTZNODE_1
  /onvif/ptz_service      GetNode(s) (continuous pan/tilt and zoom, 100
                          presets, home, preset tours), ContinuousMove
                          (Velocity, Timeout: the head stops by itself when
                          it runs out), Stop, GetPresets, SetPreset (a new
                          token Preset_N, or the one given), RemovePreset,
                          GotoPreset, GotoHomePosition, GetPresetTours,
                          OperatePresetTour
  /onvif/imaging_service  GetMoveOptions (Continuous), GetOptions
                          (AutoFocusModes AUTO, MANUAL), Move (Continuous
                          focus), Stop, SetImagingSettings
--no-ptz: a camera with neither PTZ nor Imaging; --no-timeout: a
ContinuousMove with a Timeout is refused (ter:InvalidArgVal), as some
cameras do. Every PTZ and Imaging call is a JSON line in --log: time, op,
its arguments, and the head's position after it (pan, tilt in degrees,
zoom 1..30, focus).
"""
import argparse
import base64
import hashlib
import json
import re
import socketserver
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, HTTPServer

NS = ('xmlns:s="http://www.w3.org/2003/05/soap-envelope" xmlns:tt="http://www.onvif.org/ver10/schema" '
      'xmlns:tds="http://www.onvif.org/ver10/device/wsdl" xmlns:trt="http://www.onvif.org/ver10/media/wsdl" '
      'xmlns:tptz="http://www.onvif.org/ver20/ptz/wsdl" xmlns:timg="http://www.onvif.org/ver20/imaging/wsdl"')
SOAP = '<?xml version="1.0" encoding="UTF-8"?><s:Envelope ' + NS + '><s:Body>%s</s:Body></s:Envelope>'


def log(*a):
    print("fake-onvif-ptz:", *a, flush=True)


def xml_text(body, local):
    m = re.search(r"<(?:\w+:)?%s(?:\s[^>]*)?>(.*?)</(?:\w+:)?%s>" % (local, local), body, re.S)
    return m.group(1).strip() if m else ""


def xml_attr(body, local, attr):
    m = re.search(r"<(?:\w+:)?%s\s[^>]*%s=\"([^\"]*)\"" % (local, attr), body)
    return m.group(1) if m else None


class Head:
    def __init__(self):
        self.lock = threading.Lock()
        self.pan, self.tilt, self.zoom, self.focus = 0.0, 0.0, 1.0, 0.0
        self.v = (0.0, 0.0, 0.0)            # pan, tilt, zoom velocity
        self.fv = 0.0
        self.since = time.time()
        self.until = None                   # a move's Timeout
        self.presets = {"1": ("Door", (0.0, 0.0, 1.0)), "2": ("Window", (30.0, -10.0, 2.0))}
        self.next = 3

    def settle(self, now=None):
        now = now or time.time()
        end = min(now, self.until) if self.until else now
        dt = max(0.0, end - self.since)
        self.pan = max(-180.0, min(180.0, self.pan + 30.0 * self.v[0] * dt))
        self.tilt = max(-90.0, min(90.0, self.tilt + 30.0 * self.v[1] * dt))
        self.zoom = max(1.0, min(30.0, self.zoom + 2.0 * self.v[2] * dt))
        self.focus += self.fv * dt
        if self.until and now >= self.until:
            self.v, self.until = (0.0, 0.0, 0.0), None
        self.since = now

    def where(self):
        return {"pan": round(self.pan, 1), "tilt": round(self.tilt, 1), "zoom": round(self.zoom, 2),
                "focus": round(self.focus, 2), "moving": any(self.v)}


HEAD = Head()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def reply(self, status, inner):
        raw = (SOAP % inner).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/soap+xml; charset=utf-8")
        self.send_header("Content-Length", str(len(raw)))
        self.send_header("Connection", "close")
        self.end_headers()
        try:
            self.wfile.write(raw)
        except OSError:
            pass
        self.close_connection = True

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

    def record(self, op, **kw):
        line = {"t": round(time.time(), 3), "op": op}
        line.update(kw)
        line.update(HEAD.where())
        if self.server.args.log:
            with open(self.server.args.log, "a") as f:
                f.write(json.dumps(line) + "\n")
        log(json.dumps(line))

    def do_POST(self):
        a = self.server.args
        body = self.rfile.read(int(self.headers.get("Content-Length", "0"))).decode(errors="replace")
        m = re.search(r"<s:Body>\s*<(?:\w+:)?(\w+)", body)
        op = m.group(1) if m else "?"
        base = "http://%s:%d" % (a.host, a.port)
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
        with HEAD.lock:
            HEAD.settle()
            if self.path == "/onvif/device_service":
                return self.device(op, base)
            if self.path == "/onvif/media_service" and op == "GetProfiles":
                return self.reply(200, "<trt:GetProfilesResponse>%s%s</trt:GetProfilesResponse>"
                                  % (self.profile("Profile_1", "main"), self.profile("Profile_2", "sub")))
            if self.path == "/onvif/ptz_service" and not a.no_ptz:
                return self.ptz(op, body)
            if self.path == "/onvif/imaging_service" and not a.no_ptz:
                return self.imaging(op, body)
        return self.fault("ter:ActionNotSupported", "not here")

    def device(self, op, base):
        a = self.server.args
        if op == "GetCapabilities":
            more = "" if a.no_ptz else ("<tt:PTZ><tt:XAddr>%s/onvif/ptz_service</tt:XAddr></tt:PTZ>"
                                        "<tt:Imaging><tt:XAddr>%s/onvif/imaging_service</tt:XAddr></tt:Imaging>"
                                        % (base, base))
            return self.reply(200, "<tds:GetCapabilitiesResponse><tds:Capabilities><tt:Media><tt:XAddr>"
                                   "%s/onvif/media_service</tt:XAddr></tt:Media>%s</tds:Capabilities>"
                                   "</tds:GetCapabilitiesResponse>" % (base, more))
        if op == "GetServices":
            svc = [("http://www.onvif.org/ver10/media/wsdl", "media_service")]
            if not a.no_ptz:
                svc += [("http://www.onvif.org/ver20/ptz/wsdl", "ptz_service"),
                        ("http://www.onvif.org/ver20/imaging/wsdl", "imaging_service")]
            return self.reply(200, "<tds:GetServicesResponse>%s</tds:GetServicesResponse>" % "".join(
                "<tds:Service><tds:Namespace>%s</tds:Namespace><tds:XAddr>%s/onvif/%s</tds:XAddr></tds:Service>"
                % (ns, base, path) for ns, path in svc))
        return self.fault("ter:ActionNotSupported", "not here")

    def profile(self, token, name):
        ptz = "" if self.server.args.no_ptz else (
            '<tt:PTZConfiguration token="PTZCFG_1"><tt:Name>ptz</tt:Name><tt:NodeToken>PTZNODE_1</tt:NodeToken>'
            '</tt:PTZConfiguration>')
        return ('<trt:Profiles token="%s" fixed="true"><tt:Name>%s</tt:Name>'
                '<tt:VideoSourceConfiguration token="VSC_1"><tt:Name>vs</tt:Name><tt:SourceToken>VideoSource_1'
                '</tt:SourceToken></tt:VideoSourceConfiguration>%s</trt:Profiles>' % (token, name, ptz))

    def ptz(self, op, body):
        a = self.server.args
        profile = xml_text(body, "ProfileToken")
        if op in ("GetNode", "GetNodes"):
            tours = ("<tt:Extension><tt:SupportedPresetTour><tt:MaximumNumberOfPresetTour>%d"
                     "</tt:MaximumNumberOfPresetTour></tt:SupportedPresetTour></tt:Extension>" % a.tours) if a.tours else ""
            node = ('<tptz:PTZNode token="PTZNODE_1" FixedHomePosition="false"><tt:Name>node</tt:Name>'
                    "<tt:SupportedPTZSpaces><tt:ContinuousPanTiltVelocitySpace><tt:URI>http://www.onvif.org/ver10/"
                    "tptz/PanTiltSpaces/VelocityGenericSpace</tt:URI><tt:XRange><tt:Min>-1</tt:Min><tt:Max>1</tt:Max>"
                    "</tt:XRange><tt:YRange><tt:Min>-1</tt:Min><tt:Max>1</tt:Max></tt:YRange>"
                    "</tt:ContinuousPanTiltVelocitySpace><tt:ContinuousZoomVelocitySpace><tt:URI>http://www.onvif.org/"
                    "ver10/tptz/ZoomSpaces/VelocityGenericSpace</tt:URI><tt:XRange><tt:Min>-1</tt:Min><tt:Max>1"
                    "</tt:Max></tt:XRange></tt:ContinuousZoomVelocitySpace></tt:SupportedPTZSpaces>"
                    "<tt:MaximumNumberOfPresets>100</tt:MaximumNumberOfPresets><tt:HomeSupported>true"
                    "</tt:HomeSupported>%s</tptz:PTZNode>" % tours)
            tag = "PTZNode" if op == "GetNode" else "PTZNode"
            return self.reply(200, "<tptz:%sResponse>%s</tptz:%sResponse>" % (op, node.replace("PTZNode", tag), op))
        if profile not in ("Profile_1", "Profile_2"):
            return self.fault("ter:NoProfile", "no such profile")
        if op == "ContinuousMove":
            timeout = xml_text(body, "Timeout")
            if timeout and a.no_timeout:
                self.record(op, refused="Timeout")
                return self.fault("ter:InvalidArgVal", "Timeout not supported")
            pt = (float(xml_attr(body, "PanTilt", "x") or 0), float(xml_attr(body, "PanTilt", "y") or 0))
            z = float(xml_attr(body, "Zoom", "x") or 0)
            HEAD.v = (pt[0], pt[1], z)
            m = re.match(r"PT(\d+(?:\.\d+)?)S", timeout)
            HEAD.until = time.time() + float(m.group(1)) if m else None
            self.record(op, profile=profile, x=pt[0], y=pt[1], zoom_v=z, timeout=timeout)
            return self.reply(200, "<tptz:ContinuousMoveResponse/>")
        if op == "Stop":
            HEAD.v, HEAD.until = (0.0, 0.0, 0.0), None
            self.record(op, profile=profile, pantilt=xml_text(body, "PanTilt"), zoom_s=xml_text(body, "Zoom"))
            return self.reply(200, "<tptz:StopResponse/>")
        if op == "GetPresets":
            self.record(op, profile=profile)
            return self.reply(200, "<tptz:GetPresetsResponse>%s</tptz:GetPresetsResponse>" % "".join(
                '<tptz:Preset token="%s"><tt:Name>%s</tt:Name></tptz:Preset>' % (t, n)
                for t, (n, _) in sorted(HEAD.presets.items())))
        if op == "SetPreset":
            token, name = xml_text(body, "PresetToken"), xml_text(body, "PresetName")
            if not token:
                token = "Preset_%d" % HEAD.next
                HEAD.next += 1
            elif token not in HEAD.presets:
                return self.fault("ter:NoToken", "no such preset")
            old = HEAD.presets.get(token, ("", None))[0]
            HEAD.presets[token] = (name or old or token, (HEAD.pan, HEAD.tilt, HEAD.zoom))
            self.record(op, profile=profile, token=token, name=name)
            return self.reply(200, "<tptz:SetPresetResponse><tptz:PresetToken>%s</tptz:PresetToken>"
                                   "</tptz:SetPresetResponse>" % token)
        if op == "RemovePreset":
            token = xml_text(body, "PresetToken")
            if token not in HEAD.presets:
                return self.fault("ter:NoToken", "no such preset")
            del HEAD.presets[token]
            self.record(op, profile=profile, token=token)
            return self.reply(200, "<tptz:RemovePresetResponse/>")
        if op == "GotoPreset":
            token = xml_text(body, "PresetToken")
            if token not in HEAD.presets:
                return self.fault("ter:NoToken", "no such preset")
            HEAD.v, HEAD.until = (0.0, 0.0, 0.0), None
            HEAD.pan, HEAD.tilt, HEAD.zoom = HEAD.presets[token][1]
            self.record(op, profile=profile, token=token)
            return self.reply(200, "<tptz:GotoPresetResponse/>")
        if op == "GotoHomePosition":
            HEAD.v, HEAD.until = (0.0, 0.0, 0.0), None
            HEAD.pan, HEAD.tilt, HEAD.zoom = 0.0, 0.0, 1.0
            self.record(op, profile=profile)
            return self.reply(200, "<tptz:GotoHomePositionResponse/>")
        if op == "GetPresetTours":
            return self.reply(200, "<tptz:GetPresetToursResponse>%s</tptz:GetPresetToursResponse>" % "".join(
                '<tptz:PresetTour token="Tour_%d"><tt:Name>tour %d</tt:Name></tptz:PresetTour>' % (i, i)
                for i in range(1, a.tours + 1)))
        if op == "OperatePresetTour":
            self.record(op, profile=profile, tour=xml_text(body, "PresetTourToken"),
                        operation=xml_text(body, "Operation"))
            return self.reply(200, "<tptz:OperatePresetTourResponse/>")
        return self.fault("ter:ActionNotSupported", "not here")

    def imaging(self, op, body):
        src = xml_text(body, "VideoSourceToken")
        if src != "VideoSource_1":
            return self.fault("ter:NoSource", "no such video source")
        if op == "GetMoveOptions":
            return self.reply(200, "<timg:GetMoveOptionsResponse><timg:MoveOptions><tt:Continuous><tt:Speed>"
                                   "<tt:Min>-1</tt:Min><tt:Max>1</tt:Max></tt:Speed></tt:Continuous>"
                                   "</timg:MoveOptions></timg:GetMoveOptionsResponse>")
        if op == "GetOptions":
            return self.reply(200, "<timg:GetOptionsResponse><timg:ImagingOptions><tt:Focus><tt:AutoFocusModes>AUTO"
                                   "</tt:AutoFocusModes><tt:AutoFocusModes>MANUAL</tt:AutoFocusModes></tt:Focus>"
                                   "</timg:ImagingOptions></timg:GetOptionsResponse>")
        if op == "Move":
            HEAD.fv = float(xml_text(body, "Speed") or 0)
            self.record("Imaging." + op, speed=HEAD.fv)
            return self.reply(200, "<timg:MoveResponse/>")
        if op == "Stop":
            HEAD.fv = 0.0
            self.record("Imaging." + op)
            return self.reply(200, "<timg:StopResponse/>")
        if op == "SetImagingSettings":
            self.record("Imaging." + op, autofocus=xml_text(body, "AutoFocusMode"))
            return self.reply(200, "<timg:SetImagingSettingsResponse/>")
        return self.fault("ter:ActionNotSupported", "not here")


class Server(socketserver.ThreadingMixIn, HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8899)
    ap.add_argument("--user", default="admin")
    ap.add_argument("--password", default="")
    ap.add_argument("--log", default="")
    ap.add_argument("--no-ptz", action="store_true")
    ap.add_argument("--no-timeout", action="store_true")
    ap.add_argument("--tours", type=int, default=2)
    args = ap.parse_args()
    srv = Server((args.host, args.port), Handler)
    srv.args = args
    log("PTZ camera on http://%s:%d/onvif/device_service" % (args.host, args.port))
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    sys.exit(main())
