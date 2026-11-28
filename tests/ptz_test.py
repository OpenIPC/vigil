#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""End-to-end test of PTZ through the API (docs/api.md, PTZ).

    ptz_test.py VIGILD CLIP

Starts the NETIP camera emulator with a simulated PTZ head (netip_server.py
--ptz-log) as channel 1, an empty channel 2, three fake ONVIF cameras
(fake_onvif_ptz.py: a PTZ dome as channel 3, one refusing ContinuousMove's
Timeout as 4, one without PTZ as 5), and vigild with the API on a free
port; then, over NETIP: the capabilities, a move that stops on its own when
its timeout runs out, a move stopped by the client, presets set / renamed
/ gone to / removed and their names kept in vigil.ptz.json (no password in
it), what is refused (bad values, no camera, the ptz permission); over
ONVIF: the services found, a move repeated while it lasts and stopped,
focus and auto focus, the camera's presets with a rename kept here, home,
a tour, a camera that refuses the Timeout, one without PTZ; and a camera
left moving when vigild stops being stopped. Only the standard library.
"""
import base64
import json
import os
import signal
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
# openssl passwd -6 -salt vigiltestsalt secret
HASH = ("$6$vigiltestsalt$YNf0cVVTPqcmB27N7LaoJSiqJTsS7DvT6GejoILXrgfuBwdj8rCp9"
        "lnFrtSM5TO4kEOwYmcBLCsL4TzvT5U4b1")

failures = []


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what, flush=True)
    if not cond:
        failures.append(what)
    return cond


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def api(port, method, path, body=None, user="root", password="secret"):
    req = urllib.request.Request("http://127.0.0.1:%d%s" % (port, path), method=method,
                                 data=None if body is None else json.dumps(body).encode())
    req.add_header("Authorization", "Basic " + base64.b64encode(("%s:%s" % (user, password)).encode()).decode())
    req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req, timeout=15) as r:
            return r.status, json.loads(r.read() or b"null")
    except urllib.error.HTTPError as e:
        try:
            return e.code, json.loads(e.read() or b"null")
        except ValueError:
            return e.code, None


def ptz_log(path):
    try:
        with open(path) as f:
            return [json.loads(line) for line in f if line.strip()]
    except OSError:
        return []


def wait_log(path, pred, secs=5.0):
    end = time.time() + secs
    while time.time() < end:
        lines = ptz_log(path)
        if pred(lines):
            return lines
        time.sleep(0.05)
    return ptz_log(path)


def main():
    vigild, clip = sys.argv[1], sys.argv[2]
    tmp = tempfile.mkdtemp(prefix="vigil-ptz-", dir=os.getcwd())
    api_port, cam_port = free_port(), free_port()
    onvif_ports = [free_port(), free_port(), free_port()]
    shadow = os.path.join(tmp, "shadow")
    with open(shadow, "w") as f:
        f.write("root:%s:19000:0:99999:7:::\n" % HASH)
    cfg = os.path.join(tmp, "vigil.yaml")
    with open(cfg, "w") as f:
        f.write("cameras:\n  - url: \"netip://admin:cam@127.0.0.1:%d/?ch=0\"\n    name: Dome\n"
                "  - url: \"\"\n"
                "  - url: \"rtsp://admin:onv@127.0.0.1:9/dome\"\n    name: Dome ONVIF\n"
                "    onvif_url: \"http://127.0.0.1:%d/onvif/device_service\"\n"
                "  - url: \"rtsp://admin:onv@127.0.0.1:9/old\"\n"
                "    onvif_url: \"http://127.0.0.1:%d/onvif/device_service\"\n"
                "  - url: \"rtsp://admin:onv@127.0.0.1:9/fixed\"\n"
                "    onvif_url: \"http://127.0.0.1:%d/onvif/device_service\"\n"
                "api:\n  listen: 127.0.0.1:%d\n  shadow: %s\n  users: %s\n"
                "onvif_server:\n  enabled: false\nrtsp_server:\n  enabled: false\nnetip_server:\n  enabled: false\n"
                % (cam_port, onvif_ports[0], onvif_ports[1], onvif_ports[2], api_port, json.dumps(shadow),
                   json.dumps(os.path.join(tmp, "users.yaml"))))
    log = os.path.join(tmp, "ptz.log")
    cam = subprocess.Popen([sys.executable, os.path.join(HERE, "netip_server.py"), clip, "--port", str(cam_port),
                            "--password", "cam", "--ptz-log", log],
                           stdout=open(os.path.join(tmp, "camera.log"), "w"), stderr=subprocess.STDOUT)
    olog = [os.path.join(tmp, "onvif%d.log" % i) for i in range(3)]
    fakes = [subprocess.Popen([sys.executable, os.path.join(HERE, "fake_onvif_ptz.py"), "--port", str(onvif_ports[i]),
                               "--password", "onv", "--log", olog[i]] + extra,
                              stdout=open(os.path.join(tmp, "onvif%d.out" % i), "w"), stderr=subprocess.STDOUT)
             for i, extra in enumerate(([], ["--no-timeout"], ["--no-ptz"]))]
    vlog = os.path.join(tmp, "vigild.log")
    vd = subprocess.Popen([vigild, "-f", cfg, "-o", "none", "-t", "120"], stdout=open(vlog, "w"),
                          stderr=subprocess.STDOUT)
    try:
        end = time.time() + 10
        st = 0
        while time.time() < end:
            try:
                st, _ = api(api_port, "GET", "/api/v1/status")
            except OSError:
                st = 0
            if st == 200:
                break
            time.sleep(0.2)
        if not check(st == 200, "vigild answers"):
            return
        live, end = False, time.time() + 15
        while not live and time.time() < end:
            st, b = api(api_port, "GET", "/api/v1/status")
            live = st == 200 and '"live"' in json.dumps(b)
            time.sleep(0.2)
        check(live, "the camera is live")

        # ---- what the camera can do
        st, b = api(api_port, "GET", "/api/v1/channels/1/ptz?wait=5000")
        check(st == 200 and b["state"] == "ready" and b["backend"] == "netip" and
              {"pan_tilt", "zoom", "focus", "iris", "presets", "tours"} <= set(b["capabilities"]) and
              "home" not in b["capabilities"] and b["presets"] == [] and b["tours"] == 8,
              "GET: ready over NETIP, its capabilities (%s)" % b)
        probe = ptz_log(log)
        check(len(probe) == 1 and probe[0]["cmd"] == "DirectionUp" and probe[0]["preset"] == -1,
              "the probe: one stop of nothing")

        # ---- a move that runs out
        t0 = time.time()
        st, b = api(api_port, "POST", "/api/v1/channels/1/ptz", {"move": {"pan": -1, "timeout": 400}})
        check(st == 202 and b["channel"] == 1, "POST move: 202 (%s %s)" % (st, b))
        lines = wait_log(log, lambda l: any(x["cmd"] == "DirectionLeft" and x["preset"] == -1 for x in l))
        start = [x for x in lines if x["cmd"] == "DirectionLeft" and x["preset"] == 65535]
        stop = [x for x in lines if x["cmd"] == "DirectionLeft" and x["preset"] == -1]
        check(len(start) == 1 and start[0]["step"] == 8 and start[0]["channel"] == 0,
              "DirectionLeft started at Step 8 on the camera's channel 0")
        check(len(stop) == 1, "and stopped without being asked")
        if start and stop:
            held = stop[0]["t"] - start[0]["t"]
            check(0.3 <= held <= 1.2, "after its timeout (%.2f s for 0.4 s)" % held)
            check(stop[0]["pan"] < -1.0, "the head turned left (%.1f degrees)" % stop[0]["pan"])

        # ---- a move the client stops
        n = len(ptz_log(log))
        api(api_port, "POST", "/api/v1/channels/1/ptz", {"move": {"tilt": 0.5, "zoom": 0}, "timeout": 5000})
        time.sleep(0.6)
        st, b = api(api_port, "GET", "/api/v1/channels/1/ptz")
        check(b and b["moving"], "moving while it holds")
        st, _ = api(api_port, "POST", "/api/v1/channels/1/ptz", {"stop": True})
        lines = wait_log(log, lambda l: any(x["cmd"] == "DirectionUp" and x["preset"] == -1 for x in l[n:]))
        new = lines[n:]
        check(st == 202 and [(x["cmd"], x["preset"], x["step"]) for x in new] ==
              [("DirectionUp", 65535, 4), ("DirectionUp", -1, 4)], "tilt 0.5: Step 4, stopped by the client (%s)" %
              [(x["cmd"], x["preset"]) for x in new])
        if len(new) == 2:
            check(0.4 <= new[1]["t"] - new[0]["t"] <= 1.5, "when it asked, not at the timeout")

        # ---- zoom, focus, iris
        n = len(ptz_log(log))
        api(api_port, "POST", "/api/v1/channels/1/ptz", {"move": {"zoom": 1}, "timeout": 300})
        wait_log(log, lambda l: any(x["cmd"] == "ZoomTile" and x["preset"] == -1 for x in l[n:]))
        api(api_port, "POST", "/api/v1/channels/1/ptz", {"focus": -0.5, "timeout": 300})
        wait_log(log, lambda l: any(x["cmd"] == "FocusNear" and x["preset"] == -1 for x in l[n:]))
        api(api_port, "POST", "/api/v1/channels/1/ptz", {"iris": 1, "timeout": 300})
        lines = wait_log(log, lambda l: any(x["cmd"] == "IrisLarge" and x["preset"] == -1 for x in l[n:]))
        cmds = [(x["cmd"], x["preset"]) for x in lines[n:]]
        check(cmds == [("ZoomTile", 65535), ("ZoomTile", -1), ("FocusNear", 65535), ("FocusNear", -1),
                       ("IrisLarge", 65535), ("IrisLarge", -1)], "zoom in, focus near, iris open, each stopped")
        st, b = api(api_port, "POST", "/api/v1/channels/1/ptz", {"focus": "auto"})
        check(st == 409 and "cannot" in b.get("error", ""), "focus auto: NETIP has none (%s)" % st)
        st, b = api(api_port, "POST", "/api/v1/channels/1/ptz", {"home": True})
        check(st == 409, "home: NETIP has none (%s)" % st)

        # ---- presets
        pan_at_set = ptz_log(log)[-1]["pan"]
        st, b = api(api_port, "POST", "/api/v1/channels/1/ptz", {"preset": {"set": None, "name": "Gate"}})
        check(st == 202, "a new preset (%s)" % st)
        time.sleep(0.5)
        st, b = api(api_port, "GET", "/api/v1/channels/1/ptz")
        check(b["presets"] == [{"id": "1", "name": "Gate"}], "listed as 1 Gate (%s)" % b["presets"])
        check(any(x["cmd"] == "SetPreset" and x["preset"] == 1 for x in ptz_log(log)), "SetPreset 1 sent")
        api(api_port, "POST", "/api/v1/channels/1/ptz", {"move": {"pan": 1}, "timeout": 500})
        wait_log(log, lambda l: l[-1]["cmd"] == "DirectionRight" and l[-1]["preset"] == -1)
        api(api_port, "POST", "/api/v1/channels/1/ptz", {"preset": {"goto": 1}})
        lines = wait_log(log, lambda l: l[-1]["cmd"] == "GotoPreset")
        check(lines[-1]["cmd"] == "GotoPreset" and lines[-1]["preset"] == 1 and lines[-1]["pan"] == pan_at_set,
              "goto 1: back where it was set (%.1f)" % lines[-1]["pan"])
        api(api_port, "POST", "/api/v1/channels/1/ptz", {"preset": {"set": 5}})
        api(api_port, "POST", "/api/v1/channels/1/ptz", {"preset": {"rename": "5", "name": "Road"}})
        time.sleep(0.5)
        st, b = api(api_port, "GET", "/api/v1/channels/1/ptz")
        check(b["presets"] == [{"id": "1", "name": "Gate"}, {"id": "5", "name": "Road"}],
              "set 5, renamed Road (%s)" % b["presets"])
        names = open(os.path.join(tmp, "vigil.ptz.json")).read()
        check("Road" in names and "Gate" in names and "cam@" not in names and ":cam" not in names,
              "the names kept in vigil.ptz.json, no password")
        api(api_port, "POST", "/api/v1/channels/1/ptz", {"preset": {"remove": "1"}})
        lines = wait_log(log, lambda l: l[-1]["cmd"] == "ClearPreset")
        time.sleep(0.3)
        st, b = api(api_port, "GET", "/api/v1/channels/1/ptz")
        check(lines[-1]["cmd"] == "ClearPreset" and b["presets"] == [{"id": "5", "name": "Road"}],
              "remove 1: ClearPreset, gone from the list")
        api(api_port, "POST", "/api/v1/channels/1/ptz", {"tour": {"start": 2}})
        lines = wait_log(log, lambda l: l[-1]["cmd"] == "StartTour")
        check(lines[-1]["cmd"] == "StartTour" and lines[-1]["tour"] == 2, "tour 2 started")

        # ---- refused
        for body, what in (({"move": {"pan": 2}}, "pan 2"), ({}, "nothing"), ({"spin": 1}, "an unknown member"),
                           ({"move": {"pan": 1}, "stop": True}, "a move and a stop"),
                           ({"preset": {"set": 300}}, "preset 300 on NETIP"), ({"timeout": 50, "focus": 1}, "50 ms"),
                           ({"preset": {"goto": 1, "remove": 1}}, "two preset operations")):
            st, b = api(api_port, "POST", "/api/v1/channels/1/ptz", body)
            check(st == 400, "refused: %s (%s %s)" % (what, st, b and b.get("error")))
        st, b = api(api_port, "POST", "/api/v1/channels/2/ptz", {"move": {"pan": 1}})
        check(st == 409 and "no PTZ" in b.get("error", ""), "an empty channel: 409 (%s)" % st)
        st, b = api(api_port, "GET", "/api/v1/channels/2/ptz")
        check(st == 200 and b["state"] == "off", "its state: off")
        st, _ = api(api_port, "GET", "/api/v1/channels/99/ptz")
        check(st == 404, "no such channel: 404")
        st, _ = api(api_port, "PUT", "/api/v1/channels/1/ptz", {})
        check(st == 405, "PUT: 405")
        check(api(api_port, "POST", "/api/v1/users", {"name": "anna", "group": "user", "password": "annapw"})[0]
              in (200, 201), "user anna (live, playback)")
        st, b = api(api_port, "POST", "/api/v1/channels/1/ptz", {"stop": True}, "anna", "annapw")
        check(st == 403 and "ptz" in json.dumps(b), "anna has no ptz permission: 403 (%s)" % st)
        st, b = api(api_port, "GET", "/api/v1/channels/1/ptz", None, "anna", "annapw")
        check(st == 403, "not even to read it")
        check(api(api_port, "POST", "/api/v1/groups", {"name": "op", "permissions": {"live": True, "ptz": True}})[0]
              in (200, 201) and api(api_port, "POST", "/api/v1/users", {"name": "otto", "group": "op",
                                                                        "password": "ottopw"})[0] in (200, 201),
              "user otto with ptz")
        st, _ = api(api_port, "POST", "/api/v1/channels/1/ptz", {"stop": True}, "otto", "ottopw")
        check(st == 202, "otto may (%s)" % st)

        # ---- ONVIF
        st, b = api(api_port, "GET", "/api/v1/channels/3/ptz?wait=8000")
        check(st == 200 and b["state"] == "ready" and b["backend"] == "onvif" and
              set(b["capabilities"]) == {"pan_tilt", "zoom", "focus", "focus_auto", "presets", "tours", "home"} and
              b["presets"] == [{"id": "1", "name": "Door"}, {"id": "2", "name": "Window"}] and b["tours"] == 2,
              "ONVIF: ready, its services, presets and tours (%s)" % b)
        t0 = time.time()
        api(api_port, "POST", "/api/v1/channels/3/ptz", {"move": {"pan": 0.5, "tilt": -0.25}, "timeout": 2500})
        lines = wait_log(olog[0], lambda l: any(x["op"] == "Stop" for x in l), 6)
        moves = [x for x in lines if x["op"] == "ContinuousMove"]
        check(moves and moves[0]["x"] == 0.5 and moves[0]["y"] == -0.25 and moves[0]["zoom_v"] == 0 and
              moves[0]["profile"] == "Profile_1" and moves[0]["timeout"] == "PT2S",
              "ContinuousMove on the main profile, its velocity, Timeout PT2S (%s)" % (moves[:1],))
        check(len(moves) >= 2, "sent again while it lasts (%d), so the camera's own Timeout never ends it" % len(moves))
        stops = [x for x in lines if x["op"] == "Stop"]
        check(stops and stops[0]["pantilt"] == "true" and 2.2 <= stops[0]["t"] - t0 <= 3.6,
              "Stop when the move's 2.5 s ran out (%.2f s)" % ((stops[0]["t"] - t0) if stops else -1))
        n = len(ptz_log(olog[0]))
        api(api_port, "POST", "/api/v1/channels/3/ptz", {"focus": 1, "timeout": 300})
        lines = wait_log(olog[0], lambda l: any(x["op"] == "Imaging.Stop" for x in l[n:]))
        check([(x["op"], x.get("speed")) for x in lines[n:]] == [("Imaging.Move", 1.0), ("Imaging.Stop", None)],
              "focus: Imaging Move continuous, then Stop")
        api(api_port, "POST", "/api/v1/channels/3/ptz", {"focus": "auto"})
        lines = wait_log(olog[0], lambda l: l[-1]["op"] == "Imaging.SetImagingSettings")
        check(lines[-1].get("autofocus") == "AUTO", "focus auto: SetImagingSettings AutoFocusMode AUTO")
        api(api_port, "POST", "/api/v1/channels/3/ptz", {"preset": {"set": None, "name": "Gate"}})
        lines = wait_log(olog[0], lambda l: l[-1]["op"] == "GetPresets" and any(x["op"] == "SetPreset" for x in l))
        time.sleep(0.3)
        st, b = api(api_port, "GET", "/api/v1/channels/3/ptz")
        check({"id": "Preset_3", "name": "Gate"} in b["presets"], "a new preset: the camera's token (%s)" % b["presets"])
        n = len(ptz_log(olog[0]))
        api(api_port, "POST", "/api/v1/channels/3/ptz", {"preset": {"rename": "Preset_3", "name": "Lane"}})
        time.sleep(0.5)
        st, b = api(api_port, "GET", "/api/v1/channels/3/ptz")
        check({"id": "Preset_3", "name": "Lane"} in b["presets"] and len(ptz_log(olog[0])) == n,
              "renamed here, the camera not touched")
        api(api_port, "POST", "/api/v1/channels/3/ptz", {"preset": {"goto": "2"}})
        lines = wait_log(olog[0], lambda l: l[-1]["op"] == "GotoPreset")
        check(lines[-1]["token"] == "2" and lines[-1]["pan"] == 30.0 and lines[-1]["tilt"] == -10.0,
              "goto 2: Window's position")
        api(api_port, "POST", "/api/v1/channels/3/ptz", {"preset": {"remove": "Preset_3"}})
        wait_log(olog[0], lambda l: l[-1]["op"] == "GetPresets" and l[-2]["op"] == "RemovePreset")
        time.sleep(0.3)
        st, b = api(api_port, "GET", "/api/v1/channels/3/ptz")
        check(all(p["id"] != "Preset_3" for p in b["presets"]), "removed")
        api(api_port, "POST", "/api/v1/channels/3/ptz", {"home": True})
        lines = wait_log(olog[0], lambda l: l[-1]["op"] == "GotoHomePosition")
        check(lines[-1]["op"] == "GotoHomePosition" and lines[-1]["pan"] == 0, "home")
        api(api_port, "POST", "/api/v1/channels/3/ptz", {"tour": {"start": 2}})
        lines = wait_log(olog[0], lambda l: l[-1]["op"] == "OperatePresetTour")
        check(lines[-1].get("tour") == "Tour_2" and lines[-1].get("operation") == "Start", "tour 2: Tour_2 Start")
        st, b = api(api_port, "POST", "/api/v1/channels/3/ptz", {"iris": 1})
        check(st == 409, "iris: not over ONVIF (%s)" % st)

        st, b = api(api_port, "GET", "/api/v1/channels/4/ptz?wait=8000")
        api(api_port, "POST", "/api/v1/channels/4/ptz", {"move": {"pan": 1}, "timeout": 300})
        lines = wait_log(olog[1], lambda l: any(x["op"] == "Stop" for x in l))
        ops = [(x["op"], x.get("refused"), x.get("timeout")) for x in lines if x["op"] != "GetPresets"]
        check(ops[:3] == [("ContinuousMove", "Timeout", None), ("ContinuousMove", None, ""), ("Stop", None, None)],
              "a camera refusing the Timeout: moved without one, then stopped (%s)" % ops)
        st, b = api(api_port, "GET", "/api/v1/channels/5/ptz?wait=8000")
        check(b["state"] == "none" and "no PTZ" in b["error"], "a camera without PTZ: none (%s)" % b.get("error"))

        # ---- vigild stops while the camera moves
        n = len(ptz_log(log))
        api(api_port, "POST", "/api/v1/channels/1/ptz", {"move": {"pan": -0.5, "tilt": -0.5}, "timeout": 10000})
        wait_log(log, lambda l: len(l) > n)
        vd.send_signal(signal.SIGTERM)
        vd.wait(15)
        lines = ptz_log(log)[n:]
        check([(x["cmd"], x["preset"]) for x in lines] == [("DirectionLeftDown", 65535), ("DirectionLeftDown", -1)],
              "vigild stopping: the camera stopped first (%s)" % [(x["cmd"], x["preset"]) for x in lines])
        text = open(vlog).read()
        check("cam" not in "".join(l for l in text.splitlines() if "ptz" in l and "@" in l),
              "no password in the log's PTZ lines")
    finally:
        if vd.poll() is None:
            vd.kill()
        cam.kill()
        for f in fakes:
            f.kill()
    print("%d failure(s)" % len(failures))
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
