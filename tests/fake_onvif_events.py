#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""A fake ONVIF camera's event service, for vigil's camera events tests.

    fake_onvif_events.py [--host 127.0.0.1] [--port 8899] [--user admin]
                         [--password ''] [--life 120] [--ctl UDPPORT]
                         [--script "SECS:NAME:STATE,..."] [--refs] [--no-renew]
                         [--public HOST:PORT]

Answers, over HTTP, SOAP 1.2 with a checked WS-UsernameToken (as
fake_discovery.py's ONVIF device does):
  /onvif/device_service  GetSystemDateAndTime (no login), GetCapabilities
                         (Events: XAddr /onvif/events), GetServices
  /onvif/events          CreatePullPointSubscription: a subscription at
                         /onvif/sub/N living --life s (with --refs, Axis-style
                         ReferenceParameters that every later call must send
                         back in its header); then the state of motion and
                         tamper as "Initialized" notifications
  /onvif/sub/N           PullMessages (held up to its Timeout until there is
                         something), Renew (refused with --no-renew),
                         Unsubscribe; a subscription past its time, or
                         forgotten, answers a fault
Notifications by name (NAME), each with its topic, Source and Data the way
the cameras that send them write them:
  motion   tns1:RuleEngine/CellMotionDetector/Motion  IsMotion (Hikvision)
  motion2  tns1:VideoSource/MotionAlarm               State (Dahua, XM)
  people   tns1:RuleEngine/MyRuleDetector/PeopleDetect State
  vehicle  tns1:RuleEngine/MyRuleDetector/VehicleDetect State
  face     tns1:RuleEngine/MyRuleDetector/FaceDetect   State
  field    tns1:RuleEngine/FieldDetector/ObjectsInside IsInside, Rule MyFieldDetector1
  field2   the same, Rule MyFieldDetector2
  line     tns1:RuleEngine/LineDetector/Crossed        ObjectId (a moment)
  tamper   tns1:VideoSource/GlobalSceneChange/ImagingService State
  input    tns1:Device/Trigger/DigitalInput            LogicalState
  loss     tns1:VideoSource/SignalLoss                 State
  cpu      tns1:Monitoring/ProcessorUsage              Value (not an alarm)
STATE is start, stop or "-" (line). --script plays "SECS:NAME:STATE,..." after
the first subscription; --ctl takes UDP datagrams on 127.0.0.1: "NAME STATE",
or "expire" (every subscription forgotten: a camera that rebooted). Prints
one line per call.
"""
import argparse
import base64
import hashlib
import re
import socket
import socketserver
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, HTTPServer

LOCK = threading.Lock()
COND = threading.Condition(LOCK)
SUBS = {}           # id -> {"ends": t, "queue": [xml]}
NEXT = [1]
FIRST = threading.Event()
STATE = {"motion": False, "tamper": False}

NS = ('xmlns:s="http://www.w3.org/2003/05/soap-envelope" xmlns:tt="http://www.onvif.org/ver10/schema" '
      'xmlns:tds="http://www.onvif.org/ver10/device/wsdl" xmlns:tev="http://www.onvif.org/ver10/events/wsdl" '
      'xmlns:wsnt="http://docs.oasis-open.org/wsn/b-2" xmlns:wsa5="http://www.w3.org/2005/08/addressing" '
      'xmlns:tns1="http://www.onvif.org/ver10/topics"')
SOAP = '<?xml version="1.0" encoding="UTF-8"?><s:Envelope ' + NS + '><s:Body>%s</s:Body></s:Envelope>'

NOTES = {
    "motion": ("tns1:RuleEngine/CellMotionDetector/Motion",
               [("VideoSourceConfigurationToken", "VideoSourceToken"),
                ("VideoAnalyticsConfigurationToken", "VideoAnalyticsToken"), ("Rule", "MyMotionDetectorRule")],
               "IsMotion"),
    "motion2": ("tns1:VideoSource/MotionAlarm", [("Source", "VideoSource_0")], "State"),
    "people": ("tns1:RuleEngine/MyRuleDetector/PeopleDetect", [("Source", "000")], "State"),
    "vehicle": ("tns1:RuleEngine/MyRuleDetector/VehicleDetect", [("Source", "000")], "State"),
    "face": ("tns1:RuleEngine/MyRuleDetector/FaceDetect", [("Source", "000")], "State"),
    "field": ("tns1:RuleEngine/FieldDetector/ObjectsInside",
              [("VideoSourceConfigurationToken", "VideoSourceToken"), ("Rule", "MyFieldDetector1")], "IsInside"),
    "field2": ("tns1:RuleEngine/FieldDetector/ObjectsInside",
               [("VideoSourceConfigurationToken", "VideoSourceToken"), ("Rule", "MyFieldDetector2")], "IsInside"),
    "line": ("tns1:RuleEngine/LineDetector/Crossed",
             [("VideoSourceConfigurationToken", "VideoSourceToken"), ("Rule", "MyLineDetector1")], "ObjectId"),
    "tamper": ("tns1:VideoSource/GlobalSceneChange/ImagingService", [("Source", "VideoSource_0")], "State"),
    "input": ("tns1:Device/Trigger/DigitalInput", [("InputToken", "1")], "LogicalState"),
    "loss": ("tns1:VideoSource/SignalLoss", [("Source", "VideoSource_0")], "State"),
    "cpu": ("tns1:Monitoring/ProcessorUsage", [("Token", "CPU")], "Value"),
}


def log(*a):
    print("fake-onvif:", *a, flush=True)


def utc(t=None):
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(t if t is not None else time.time()))


def xml_text(body, local):
    m = re.search(r"<(?:\w+:)?%s(?:\s[^>]*)?>(.*?)</(?:\w+:)?%s>" % (local, local), body, re.S)
    return m.group(1).strip() if m else ""


def note(name, state, op="Changed"):
    topic, source, item = NOTES[name]
    if item == "ObjectId":
        value = "7"
    elif item == "Value":
        value = "0.27"
    else:
        value = "true" if state == "start" else "false"
    src = "".join('<tt:SimpleItem Name="%s" Value="%s"/>' % kv for kv in source)
    return ('<wsnt:NotificationMessage><wsnt:Topic Dialect="http://www.onvif.org/ver10/tev/topicExpression/'
            'ConcreteSet">%s</wsnt:Topic><wsnt:Message><tt:Message UtcTime="%s" PropertyOperation="%s">'
            '<tt:Source>%s</tt:Source><tt:Data><tt:SimpleItem Name="%s" Value="%s"/></tt:Data></tt:Message>'
            '</wsnt:Message></wsnt:NotificationMessage>' % (topic, utc(), op, src, item, value))


def notify(name, state):
    if name not in NOTES:
        log("unknown notification", name)
        return
    if name in STATE:
        STATE[name] = state == "start"
    with COND:
        for s in SUBS.values():
            s["queue"].append(note(name, state))
        COND.notify_all()
    log("notify %s %s to %d subscription(s)" % (name, state, len(SUBS)))


def expire():
    with COND:
        SUBS.clear()
        COND.notify_all()
    log("every subscription forgotten")


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
            pass                # the client gave up (vigild stopping mid-PullMessages)
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

    def do_POST(self):
        a = self.server.args
        body = self.rfile.read(int(self.headers.get("Content-Length", "0"))).decode(errors="replace")
        m = re.search(r"<s:Body>\s*<(?:\w+:)?(\w+)", body)
        op = m.group(1) if m else "?"
        base = "http://%s" % a.public if a.public else "http://%s:%d" % (a.host, a.port)
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
        if self.path == "/onvif/device_service":
            log("device", op)
            if op == "GetCapabilities":
                return self.reply(200, "<tds:GetCapabilitiesResponse><tds:Capabilities><tt:Events><tt:XAddr>"
                                       "%s/onvif/events</tt:XAddr><tt:WSSubscriptionPolicySupport>false"
                                       "</tt:WSSubscriptionPolicySupport><tt:WSPullPointSupport>true"
                                       "</tt:WSPullPointSupport></tt:Events></tds:Capabilities>"
                                       "</tds:GetCapabilitiesResponse>" % base)
            if op == "GetServices":
                return self.reply(200, "<tds:GetServicesResponse><tds:Service><tds:Namespace>"
                                       "http://www.onvif.org/ver10/events/wsdl</tds:Namespace><tds:XAddr>"
                                       "%s/onvif/events</tds:XAddr></tds:Service></tds:GetServicesResponse>" % base)
            return self.fault("ter:ActionNotSupported", "not here")
        if self.path == "/onvif/events":
            if op != "CreatePullPointSubscription":
                return self.fault("ter:ActionNotSupported", "not here")
            with COND:
                sid = NEXT[0]
                NEXT[0] += 1
                init = [note("motion", "start" if STATE["motion"] else "stop", "Initialized"),
                        note("tamper", "start" if STATE["tamper"] else "stop", "Initialized")]
                SUBS[sid] = {"ends": time.time() + a.life, "queue": init}
            log("CreatePullPointSubscription -> sub/%d" % sid)
            FIRST.set()
            refs = ('<wsa5:ReferenceParameters><dom0:SubscriptionId xmlns:dom0="http://www.axis.com/2009/event">'
                    '%d</dom0:SubscriptionId></wsa5:ReferenceParameters>' % sid) if a.refs else ""
            now = time.time()
            return self.reply(200, "<tev:CreatePullPointSubscriptionResponse><tev:SubscriptionReference>"
                                   "<wsa5:Address>%s/onvif/sub/%d</wsa5:Address>%s</tev:SubscriptionReference>"
                                   "<wsnt:CurrentTime>%s</wsnt:CurrentTime><wsnt:TerminationTime>%s"
                                   "</wsnt:TerminationTime></tev:CreatePullPointSubscriptionResponse>"
                              % (base, sid, refs, utc(now), utc(now + a.life)))
        m = re.match(r"/onvif/sub/(\d+)$", self.path)
        if not m:
            return self.fault("ter:InvalidArgVal", "no such service", 404)
        sid = int(m.group(1))
        if a.refs and ("SubscriptionId" not in body or ">%d<" % sid not in body):
            log("sub/%d %s without its ReferenceParameters" % (sid, op))
            return self.fault("ter:InvalidArgVal", "reference parameters missing")
        with COND:
            sub = SUBS.get(sid)
            if sub and sub["ends"] < time.time():
                del SUBS[sid]
                sub = None
        if not sub:
            log("sub/%d %s: no such subscription" % (sid, op))
            return self.fault("ter:InvalidArgVal", "no such subscription")
        if op == "PullMessages":
            m2 = re.search(r"PT(\d+)S", xml_text(body, "Timeout"))
            wait = int(m2.group(1)) if m2 else 5
            end = time.time() + wait
            with COND:
                while sid in SUBS and not SUBS[sid]["queue"] and time.time() < end:
                    COND.wait(end - time.time())
                if sid not in SUBS:
                    got = None
                else:
                    got = SUBS[sid]["queue"]
                    SUBS[sid]["queue"] = []
                    ends = SUBS[sid]["ends"]
            if got is None:
                log("sub/%d PullMessages: forgotten while waiting" % sid)
                return self.fault("ter:InvalidArgVal", "no such subscription")
            if got:
                log("sub/%d PullMessages -> %d" % (sid, len(got)))
            now = time.time()
            return self.reply(200, "<tev:PullMessagesResponse><tev:CurrentTime>%s</tev:CurrentTime>"
                                   "<tev:TerminationTime>%s</tev:TerminationTime>%s</tev:PullMessagesResponse>"
                              % (utc(now), utc(ends), "".join(got)))
        if op == "Renew":
            if a.no_renew:
                log("sub/%d Renew refused" % sid)
                return self.fault("ter:ActionNotSupported", "Renew not supported")
            with COND:
                if sid in SUBS:
                    SUBS[sid]["ends"] = time.time() + a.life
            log("sub/%d Renew" % sid)
            now = time.time()
            return self.reply(200, "<wsnt:RenewResponse><wsnt:TerminationTime>%s</wsnt:TerminationTime>"
                                   "<wsnt:CurrentTime>%s</wsnt:CurrentTime></wsnt:RenewResponse>"
                              % (utc(now + a.life), utc(now)))
        if op == "Unsubscribe":
            with COND:
                SUBS.pop(sid, None)
                COND.notify_all()
            log("sub/%d Unsubscribe" % sid)
            return self.reply(200, "<wsnt:UnsubscribeResponse/>")
        return self.fault("ter:ActionNotSupported", op)


class Server(socketserver.ThreadingMixIn, HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def ctl(port):
    u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    u.bind(("127.0.0.1", port))
    while True:
        words = u.recv(256).decode(errors="replace").split()
        if words == ["expire"]:
            expire()
        elif len(words) >= 2:
            notify(words[0], words[1])


def script(text):
    FIRST.wait()
    t0 = time.monotonic()
    for step in text.split(","):
        parts = step.strip().split(":")
        if len(parts) != 3:
            continue
        time.sleep(max(0.0, t0 + float(parts[0]) - time.monotonic()))
        if parts[1] == "expire":
            expire()
        else:
            notify(parts[1], parts[2])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8899)
    ap.add_argument("--user", default="admin")
    ap.add_argument("--password", default="")
    ap.add_argument("--life", type=int, default=120)
    ap.add_argument("--ctl", type=int, default=0)
    ap.add_argument("--script", default="")
    ap.add_argument("--refs", action="store_true")
    ap.add_argument("--no-renew", action="store_true")
    ap.add_argument("--public", default="", help="HOST:PORT the addresses in the answers name (behind a NAT)")
    args = ap.parse_args()
    srv = Server((args.host, args.port), Handler)
    srv.args = args
    if args.ctl:
        threading.Thread(target=ctl, args=(args.ctl,), daemon=True).start()
    if args.script:
        threading.Thread(target=script, args=(args.script,), daemon=True).start()
    log("events on http://%s:%d/onvif/device_service" % (args.host, args.port))
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    sys.exit(main())
