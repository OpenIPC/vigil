#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""End-to-end test of the cameras' events (src/core/cam_events.h).

    events_test.py VIGILD CLIP

Two fake cameras on 127.0.0.1: netip_server.py, which streams CLIP and
pushes NETIP alarms on command (--alarm-ctl), and fake_onvif_events.py,
an ONVIF event service whose notifications come on command (--ctl). vigild
runs with two channels on the NETIP camera: CAM1 takes its events from its
NETIP connection (events auto), CAM2 from the ONVIF service (events onvif,
onvif_url). Then, through the API: the status's events and the WebSocket's
event messages as alarms start and stop; two rules of one type held until
both end; a line crossing held for a moment; the ONVIF camera forgetting
its subscriptions (a reboot) and vigild subscribing again, what was on
ended; renewal; the API's own start and stop; and CAM1's events switched
off without reconnecting its streams.
"""
import json
import os
import socket
import subprocess
import sys
import tempfile
import time

from api_test import HASH, Api, WebSocket, check, failures, free_port, wait_for

HERE = os.path.dirname(os.path.abspath(__file__))
CAM_PASS = "camsecret"


def free_udp_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def main():
    vigild, clip = sys.argv[1], sys.argv[2]
    tmp = tempfile.mkdtemp(prefix="vigil-events-", dir=os.getcwd())
    api_port, cam_port, onvif_port = free_port(), free_port(), free_port()
    netip_ctl, onvif_ctl = free_udp_port(), free_udp_port()
    shadow = os.path.join(tmp, "shadow")
    with open(shadow, "w") as f:
        f.write("root:%s:19000:0:99999:7:::\n" % HASH)
    cfg_path = os.path.join(tmp, "vigil.yaml")
    cam = "netip://admin:%s@127.0.0.1:%d/" % (CAM_PASS, cam_port)
    with open(cfg_path, "w") as f:
        f.write("cameras:\n"
                "  - url: %s\n"
                "  - url: %s\n"
                "    events: onvif\n"
                "    onvif_url: http://127.0.0.1:%d/onvif/device_service\n"
                "api:\n"
                "  listen: 127.0.0.1:%d\n"
                "  auth: on\n"
                "  user: root\n"
                "  shadow: %s\n"
                "  users: %s\n"
                "onvif_server:\n  enabled: false\n"
                "netip_server:\n  enabled: false\n"
                "rtsp_server:\n  enabled: false\n"
                "web:\n  listen: \"\"\n"
                % (json.dumps(cam), json.dumps(cam), onvif_port, api_port, json.dumps(shadow),
                   json.dumps(os.path.join(tmp, "users.yaml"))))

    srv_log = open(os.path.join(tmp, "netip.log"), "w")
    srv = subprocess.Popen([sys.executable, os.path.join(HERE, "netip_server.py"), clip, "--port", str(cam_port),
                            "--password", CAM_PASS, "--alarm-ctl", str(netip_ctl)],
                           stdout=srv_log, stderr=subprocess.STDOUT)
    onvif_log_path = os.path.join(tmp, "onvif.log")
    onvif_log = open(onvif_log_path, "w")
    onv = subprocess.Popen([sys.executable, os.path.join(HERE, "fake_onvif_events.py"), "--port", str(onvif_port),
                            "--password", CAM_PASS, "--ctl", str(onvif_ctl), "--life", "35", "--refs"],
                           stdout=onvif_log, stderr=subprocess.STDOUT)
    log_path = os.path.join(tmp, "vigild.log")
    log = open(log_path, "w")
    time.sleep(0.5)
    vd = subprocess.Popen([vigild, "-f", cfg_path, "-t", "120"], stdout=log, stderr=subprocess.STDOUT)
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    def netip(cmd):
        udp.sendto(cmd.encode(), ("127.0.0.1", netip_ctl))

    def onvif(cmd):
        udp.sendto(cmd.encode(), ("127.0.0.1", onvif_ctl))

    def vlog():
        return open(log_path).read()

    def onvif_said():
        return open(onvif_log_path).read()

    ws = None
    try:
        if not check(wait_for(lambda: "api: listening" in vlog(), 10), "vigild serves the API"):
            return
        api = Api(api_port)

        def events(ch):
            st, _, doc = api.json("GET", "/api/v1/status")
            if st != 200:
                return None
            chans = doc["channels"]
            return chans[ch - 1]["events"] if len(chans) >= ch else None

        def held(ch, want):
            return lambda: events(ch) == want

        def ws_event(ch, name, state, detail=None):
            def f():
                return ws.find(lambda m: m["type"] == "event" and m["data"]["camera"] == ch and
                               m["data"].get("event") == name and m["data"].get("state") == state and
                               (detail is None or m["data"].get("detail") == detail))
            return f

        def live(ch):
            st, _, doc = api.json("GET", "/api/v1/status")
            return st == 200 and len(doc["channels"]) >= ch and doc["channels"][ch - 1]["live"]

        check(wait_for(lambda: live(1) and live(2), 15), "both channels live")
        check(wait_for(lambda: "CAM2: ONVIF events from http://127.0.0.1:%d/onvif/events" % onvif_port in vlog(), 10),
              "CAM2 subscribed to the ONVIF camera's events")
        check(wait_for(lambda: "alarm subscription" in open(srv_log.name).read(), 5),
              "CAM1 asked the NETIP camera for its alarms")
        ws = WebSocket(api_port, api.auth)
        check(ws.status == 101, "WebSocket up")
        time.sleep(1)

        # ---- NETIP: CAM1's alarm, start to stop; CAM2 does not take NETIP's
        netip("VideoMotion Start")
        check(wait_for(held(1, ["motion"]), 3), "NETIP VideoMotion Start: CAM1 shows motion")
        check(wait_for(ws_event(1, "motion", "start", "VideoMotion"), 3), "WebSocket: CAM1 motion start, VideoMotion")
        time.sleep(CHANNEL_EVENT_HOLD + 1)
        check(events(1) == ["motion"], "held while it goes on, past the 5 s of a moment")
        check(events(2) == [], "CAM2 (events onvif) took no NETIP alarm")
        netip("HumanDetect Start")
        check(wait_for(held(1, ["motion", "human"]), 3), "NETIP HumanDetect: motion and human")
        netip("VideoMotion Stop")
        netip("HumanDetect Stop")
        check(wait_for(held(1, []), 3), "both Stop: nothing held at once")
        check(wait_for(ws_event(1, "motion", "stop"), 3) and wait_for(ws_event(1, "human", "stop"), 3),
              "WebSocket: their stops")
        netip("VideoLoss Start 1")
        time.sleep(1)
        check(events(1) == [], "another channel's alarm (Channel 1) not taken")

        # ---- ONVIF: CAM2's notifications
        onvif("people start")
        check(wait_for(held(2, ["human"]), 8), "ONVIF PeopleDetect true: CAM2 shows human")
        check(wait_for(ws_event(2, "human", "start", "RuleEngine/MyRuleDetector/PeopleDetect"), 3),
              "WebSocket: CAM2 human start, its topic")
        onvif("field start")
        onvif("field2 start")
        check(wait_for(held(2, ["human", "intrusion"]), 8), "two field rules: intrusion")
        onvif("field stop")
        time.sleep(1.5)
        check(events(2) == ["human", "intrusion"], "one rule over, the other holds it")
        onvif("field2 stop")
        check(wait_for(held(2, ["human"]), 8), "both over: intrusion off")
        onvif("line -")
        check(wait_for(held(2, ["human", "line_crossing"]), 8), "a line crossed: held for a moment")
        check(wait_for(held(2, ["human"]), CHANNEL_EVENT_HOLD + 3), "... and over by itself")
        check(wait_for(ws_event(2, "line_crossing", "stop"), 3), "WebSocket: line crossing stop from the ticker")
        onvif("cpu start")
        onvif("input start")
        check(wait_for(held(2, ["human", "alarm_input"]), 8), "digital input; the processor's load is no alarm")
        onvif("input stop")
        check(wait_for(held(2, ["human"]), 8), "digital input off")

        # ---- renewal before the subscription's 35 s run out
        check(wait_for(lambda: "Renew" in onvif_said(), 40), "the subscription renewed")

        # ---- the camera forgets its subscriptions: vigild subscribes again
        onvif("expire")
        check(wait_for(held(2, []), 15), "the subscription lost: what it held ended")
        check(wait_for(lambda: onvif_said().count("CreatePullPointSubscription ->") >= 2, 15),
              "subscribed again")
        check(wait_for(lambda: vlog().count("CAM2: ONVIF events from") >= 2, 10), "said so")
        onvif("tamper start")
        check(wait_for(held(2, ["tamper"]), 8), "the new subscription's events come (tamper)")
        onvif("tamper stop")
        check(wait_for(held(2, []), 8), "tamper off")

        # ---- the API's own: start holds until its stop
        st, _, doc = api.json("POST", "/api/v1/channels/1/events", {"events": ["other"], "state": "start"})
        check(st == 200 and doc["events"] == ["other"], "API start: held (%s %s)" % (st, doc))
        time.sleep(CHANNEL_EVENT_HOLD + 1)
        check(events(1) == ["other"], "API start: still held after 5 s")
        st, _, doc = api.json("POST", "/api/v1/channels/1/events", {"events": ["other"], "state": "stop"})
        check(st == 200 and doc["events"] == [], "API stop: over (%s %s)" % (st, doc))
        st, _, err = api.json("POST", "/api/v1/channels/1/events", {"events": ["motion"], "state": "maybe"})
        check(st == 400, "API: an unknown state refused (%s)" % st)
        st, _, err = api.json("POST", "/api/v1/channels/1/events", {"events": ["tamper", "video_loss"]})
        check(st == 200 and err["events"] == ["video_loss", "tamper"], "API: the new names, a moment (%s)" % err)

        # ---- CAM1's events off: no reconnect, NETIP alarms no longer shown
        st, _, cams = api.json("GET", "/api/v1/config/cameras")
        cams[0]["events"] = "off"
        online_before = vlog().count("CAM1 127.0.0.1: online")
        applied_before = vlog().count("applied")
        st, _, _ = api.json("PUT", "/api/v1/config/cameras", cams)
        check(st == 200, "CAM1 events off saved (%s)" % st)
        check(wait_for(lambda: vlog().count("applied") > applied_before, 5), "applied")
        time.sleep(1)
        netip("VideoMotion Start")
        time.sleep(1.5)
        check(events(1) in ([], ["video_loss", "tamper"]) and "motion" not in (events(1) or []),
              "events off: the NETIP alarm not shown (%s)" % events(1))
        check(vlog().count("CAM1 127.0.0.1: online") == online_before, "the streams were not reconnected")
        netip("VideoMotion Stop")
    finally:
        if ws:
            ws.close()
        vd.terminate()
        try:
            vd.wait(10)
        except subprocess.TimeoutExpired:
            vd.kill()
        for p in (srv, onv):
            p.terminate()
            p.wait(5)
        log.close()
        text = open(log_path).read()
        print("---- vigild\n" + "\n".join(l for l in text.splitlines() if "CAM" in l or "events" in l))
        print("---- fake ONVIF camera\n" + open(onvif_log_path).read())
    if failures:
        print("FAILED: %d" % len(failures))
        sys.exit(1)
    print("PASS")


CHANNEL_EVENT_HOLD = 5

if __name__ == "__main__":
    main()
