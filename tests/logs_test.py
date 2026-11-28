#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""End-to-end test of the logs (docs/management.md, "Logs"; docs/api.md).

    logs_test.py VIGILD CLIP

vigild records into the build tree (record.allow_rootfs), so its alarm and
operation logs go to vigil-logs/ beside the recording directory, and their
flash copies to vigil-logs/ beside vigil.yaml (here: the same directory,
other file names). Real actions fill the operation log (failed logins and a
lockout, settings saved, users added, changed and deleted, an export), a
NETIP camera emulator and the API's POST .../events the alarm log (motion
start and stop, the camera going offline and back); then filters, paging,
permissions, CSV, no secret anywhere, and two restarts: one without the
disk (the flash copy), one with it again (no doubles).
"""
import base64
import http.client
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
CAM_PASS = "camsecret77"
SECRETS = [CAM_PASS, "secret", "verapw1", "verapw2", "guess-pw"]

failures = []


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what, flush=True)
    if not cond:
        failures.append(what)
    return cond


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def wait_for(pred, secs, step=0.2):
    end = time.time() + secs
    while time.time() < end:
        v = pred()
        if v:
            return v
        time.sleep(step)
    return None


class Api:
    def __init__(self, port, user="root", password="secret"):
        self.port = port
        tok = base64.b64encode(("%s:%s" % (user, password)).encode()).decode()
        self.auth = "Basic " + tok

    def call(self, method, path, body=None, src=None):
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=10,
                                          source_address=(src, 0) if src else None)
        data = None if body is None else json.dumps(body).encode()
        hdrs = {"Authorization": self.auth}
        if data is not None:
            hdrs["Content-Type"] = "application/json"
        conn.request(method, path, body=data, headers=hdrs)
        r = conn.getresponse()
        raw = r.read()
        conn.close()
        return r.status, dict(r.getheaders()), raw

    def json(self, method, path, body=None):
        st, hdr, raw = self.call(method, path, body)
        try:
            return st, json.loads(raw) if raw else None
        except ValueError:
            return st, None


def main():
    vigild, clip = sys.argv[1], sys.argv[2]
    tmp = tempfile.mkdtemp(prefix="vigil-logs-", dir=os.getcwd())
    shadow = os.path.join(tmp, "shadow")
    with open(shadow, "w") as f:
        f.write("root:%s:19000:0:99999:7:::\n" % HASH)
    cfg_path = os.path.join(tmp, "vigil.yaml")
    users_path = os.path.join(tmp, "vigil.users.yaml")
    rec = os.path.join(tmp, "rec")
    os.mkdir(rec)
    logdir = os.path.join(tmp, "vigil-logs")
    api_port, cam_port = free_port(), free_port()

    def write_config(record_path):
        with open(cfg_path, "w") as f:
            f.write("cameras:\n"
                    "  - url: \"netip://admin:%s@127.0.0.1:%d/\"\n"
                    "    name: Gate\n"
                    "record:\n"
                    "  path: %s\n"
                    "  allow_rootfs: yes\n"
                    "api:\n"
                    "  listen: 127.0.0.1:%d\n"
                    "  auth: on\n"
                    "  user: root\n"
                    "  shadow: %s\n"
                    "  users: %s\n"
                    "rtsp_server:\n  enabled: false\n"
                    "onvif_server:\n  enabled: false\n"
                    "netip_server:\n  enabled: false\n"
                    "web:\n  enabled: false\n" % (CAM_PASS, cam_port, json.dumps(record_path), api_port,
                                                  json.dumps(shadow), json.dumps(users_path)))

    def start(name):
        log_path = os.path.join(tmp, name + ".log")
        log = open(log_path, "w")
        vd = subprocess.Popen([vigild, "-f", cfg_path, "-t", "120"], stdout=log, stderr=subprocess.STDOUT)
        ok = wait_for(lambda: "api: listening" in open(log_path).read(), 10)
        return vd, log, log_path, ok

    def stop(vd, log):
        vd.send_signal(signal.SIGTERM)
        try:
            vd.wait(15)
        except subprocess.TimeoutExpired:
            vd.kill()
        log.close()

    def cam_start():
        return subprocess.Popen([sys.executable, os.path.join(HERE, "netip_server.py"), clip,
                                 "--port", str(cam_port), "--password", CAM_PASS],
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    write_config(rec)
    cam = cam_start()
    vd, log, log_path, ok = start("vigild1")
    api = Api(api_port)
    try:
        if not check(ok, "vigild serves the API"):
            return
        wait_for(lambda: api.json("GET", "/api/v1/status")[1]["channels"][0]["live"], 15)

        # ---- the operation log from real actions
        st, _, _ = Api(api_port, password="guess-pw").call("GET", "/api/v1/status")
        check(st == 401, "a wrong password (%s)" % st)
        for _ in range(5):
            Api(api_port, user="mallory", password="guess-pw").call("GET", "/api/v1/status", src="127.0.0.7")
        st, _, _ = Api(api_port, user="mallory", password="guess-pw").call("GET", "/api/v1/status", src="127.0.0.7")
        check(st == 429, "locked out (%s)" % st)
        st, _ = api.json("PUT", "/api/v1/config/record", {"path": rec, "segment": 90, "allow_rootfs": True})
        check(st == 200, "record saved (%s)" % st)
        st, _ = api.json("POST", "/api/v1/users", {"name": "vera", "group": "user", "password": "verapw1"})
        check(st in (200, 201), "vera added (%s)" % st)
        st, _ = api.json("PUT", "/api/v1/users/vera", {"password": "verapw2"})
        check(st == 200, "vera's password changed (%s)" % st)
        st, _ = api.json("POST", "/api/v1/users", {"name": "kim", "group": "user", "password": "kimpw1"})
        st, _ = api.json("DELETE", "/api/v1/users/kim")
        check(st == 200, "kim deleted (%s)" % st)
        cams = api.json("GET", "/api/v1/config/cameras")[1]
        st, _ = api.json("PUT", "/api/v1/config/cameras", cams)
        check(st == 200, "the cameras saved unchanged (%s)" % st)

        st, ops = api.json("GET", "/api/v1/logs/operations?limit=200")
        check(st == 200 and ops["disk"] == logdir, "operations: 200, kept in %s (%s)" % (ops and ops["disk"], st))
        acts = [(e["action"], e["user"], e["source"], e["detail"]) for e in ops["entries"]]

        def has(action, user=None, detail=None, source=None):
            return any(a == action and (user is None or u == user) and (source is None or s == source) and
                       (detail is None or detail in d) for a, u, s, d in acts)
        check(has("start", "", "vigild "), "vigild's start")
        check(has("login", "root", "from 127.0.0.1", "api"), "root's API login")
        check(has("login_failed", "root", "from 127.0.0.1"), "root's wrong password")
        check(has("login_failed", "mallory", "from 127.0.0.7, 5 in a row") and has("lockout", None, "from 127.0.0.7"),
              "mallory's failures and the lockout")
        check(has("config_saved", "root", "record: segment", "api"), "record saved: the field's name")
        check(has("config_saved", "root", "cameras: unchanged"), "the cameras saved: nothing changed")
        check(has("user_added", "root", "vera (group user)") and has("user_changed", "root", "vera: password") and
              has("user_deleted", "root", "kim"), "users added, changed, deleted")
        check(acts and acts[0][0] in ("user_deleted", "config_saved", "login"), "newest first (%s)" % (acts[:1],))

        # filters and paging
        st, f = api.json("GET", "/api/v1/logs/operations?action=login_failed&user=mallory")
        check(st == 200 and f["count"] == 5 and all(e["user"] == "mallory" for e in f["entries"]),
              "filter by action and user (%s)" % (f and f["count"]))
        st, p1 = api.json("GET", "/api/v1/logs/operations?limit=3")
        st2, p2 = api.json("GET", "/api/v1/logs/operations?limit=3&offset=3")
        check(p1["more"] and p1["count"] == 3 and p2["entries"][0]["id"] != p1["entries"][2]["id"] and
              p2["entries"][0]["id"] == ops["entries"][3]["id"], "paging: offset and more")
        st, f = api.json("GET", "/api/v1/logs/operations?q=VERA")
        check(st == 200 and f["count"] >= 2, "text search, any case (%s)" % (f and f["count"]))
        st, f = api.json("GET", "/api/v1/logs/operations?range=today")
        check(st == 200 and f["count"] == len(ops["entries"]), "today: everything")
        st, f = api.json("GET", "/api/v1/logs/operations?to=2000-01-01")
        check(st == 200 and f["count"] == 0, "before 2000: nothing")
        st, _ = api.json("GET", "/api/v1/logs/operations?from=yesterday")
        check(st == 400, "a bad time: 400 (%s)" % st)

        # ---- the alarm log: a camera's events, offline and back
        st, _ = api.json("POST", "/api/v1/channels/1/events", {"events": ["motion"]})
        check(st == 200, "motion reported (%s)" % st)

        def alarms(q=""):
            return api.json("GET", "/api/v1/logs/alarms" + q)[1]["entries"]
        check(wait_for(lambda: [e for e in alarms() if e["type"] == "motion" and e["state"] == "start" and
                                e["channel"] == 1], 5), "motion start")
        check(wait_for(lambda: [e for e in alarms() if e["type"] == "motion" and e["state"] == "stop"], 10),
              "motion stop, once its hold ran out")
        cam.terminate()
        cam.wait()
        check(wait_for(lambda: [e for e in alarms("?type=offline") if e["state"] == "start" and e["channel"] == 1],
                       20), "the camera offline")
        cam = cam_start()
        check(wait_for(lambda: [e for e in alarms("?type=offline&channel=1") if e["state"] == "stop"], 30),
              "and back")
        st, a = api.json("GET", "/api/v1/logs/alarms?type=motion,offline&channel=2")
        check(st == 200 and a["count"] == 0, "another channel: none")

        # permissions: a viewer reads the alarms, not the rest
        vera = Api(api_port, user="vera", password="verapw2")
        st, _ = vera.json("GET", "/api/v1/logs/alarms")
        st2, e2 = vera.json("GET", "/api/v1/logs/operations")
        st3, _ = vera.json("GET", "/api/v1/logs/system")
        check(st == 200 and st2 == 403 and e2["needs"] == "system" and st3 == 403,
              "viewer: alarms 200, operations and system 403 (%s %s %s)" % (st, st2, st3))
        st, _ = api.json("POST", "/api/v1/logs/alarms")
        check(st == 405, "read only: POST 405 (%s)" % st)

        # the system log (no logread on a host: /var/log/messages or 503)
        st, sl = api.json("GET", "/api/v1/logs/system?lines=5")
        check(st in (200, 503) and (st == 503 or len(sl["lines"]) <= 5), "the system log: %s" % st)
        st, _ = api.json("GET", "/api/v1/logs/system?source=nope")
        check(st == 400, "unknown source: 400 (%s)" % st)

        # exports
        st, hdr, raw = api.call("GET", "/api/v1/logs/operations/export?format=csv")
        text = raw.decode()
        lines = text.splitlines()
        check(st == 200 and hdr.get("Content-Type", "").startswith("text/csv") and
              "attachment" in hdr.get("Content-Disposition", "") and
              lines[0] == "time,channel,state,source,user,type,detail,id" and len(lines) > 10,
              "CSV export: %d lines" % len(lines))
        st, hdr, raw = api.call("GET", "/api/v1/logs/alarms/export?format=text&type=offline")
        check(st == 200 and b"CAM1 offline start" in raw, "text export, filtered")
        st, s = api.json("GET", "/api/v1/logs")
        check(st == 200 and "motion" in s["alarm_types"] and "offline" in s["alarm_types"] and
              "config_import" in s["actions"] and s["operations"]["disk"] == logdir, "the summary")

        # no secret in any answer
        everything = b""
        for path in ("/api/v1/logs/operations?limit=1000", "/api/v1/logs/alarms?limit=1000",
                     "/api/v1/logs/operations/export", "/api/v1/logs/alarms/export"):
            everything += api.call("GET", path)[2]
        check(not any(sec.encode() in everything for sec in SECRETS), "no password in the logs' answers")
    finally:
        stop(vd, log)

    # ---- on the disk and on the flash
    disk_ops = os.path.join(logdir, "operations.log")
    flash_ops = os.path.join(logdir, "operations-recent.log")
    check(os.path.exists(disk_ops) and os.path.exists(os.path.join(logdir, "alarms.log")), "on the disk")
    check(os.path.exists(flash_ops) and "\tstop\t" in open(flash_ops).read(),
          "the flash copy, written at the clean stop")
    check(not os.path.exists(os.path.join(logdir, "alarms-recent.log")),
          "no flash copy of the alarms while a disk has them")
    blob = b""
    for name in os.listdir(logdir):
        blob += open(os.path.join(logdir, name), "rb").read()
    check(not any(sec.encode() in blob for sec in SECRETS), "no password in the files")
    check(oct(os.stat(disk_ops).st_mode & 0o777) == "0o600", "the files: 0600")
    n_disk = len(open(disk_ops).read().splitlines())

    # ---- a restart without the disk: the flash copy
    write_config("")
    vd, log, log_path, ok = start("vigild2")
    try:
        check(ok, "restarted without a recording disk")
        st, ops2 = api.json("GET", "/api/v1/logs/operations?limit=500")
        acts2 = [e["action"] for e in ops2["entries"]]
        check(st == 200 and ops2["disk"] == "" and "user_deleted" in acts2 and "lockout" in acts2 and
              acts2.count("start") == 2, "the last ones are back from the flash copy (%d)" % len(acts2))
        api.json("PUT", "/api/v1/config/ui", {"language": "en"})
    finally:
        stop(vd, log)

    # ---- the disk again: what happened meanwhile is added, nothing twice
    write_config(rec)
    vd, log, log_path, ok = start("vigild3")
    try:
        check(ok, "restarted with the disk")
        time.sleep(1.5)
        st, ops3 = api.json("GET", "/api/v1/logs/operations?limit=1000")
        ids = [e["id"] for e in ops3["entries"]]
        check(st == 200 and ops3["disk"] == logdir and len(ids) == len(set(ids)), "no doubles in the answer")
        check(any(e["action"] == "config_saved" and e["detail"].startswith("ui:") for e in ops3["entries"]),
              "the save made without the disk is there")
    finally:
        stop(vd, log)
        cam.terminate()
        cam.wait()
    file_ids = [l.split("\t")[0] for l in open(disk_ops).read().splitlines()]
    check(len(file_ids) == len(set(file_ids)) and len(file_ids) > n_disk, "the disk's file: nothing twice (%d -> %d)"
          % (n_disk, len(file_ids)))

    if failures:
        print("%d failure(s)" % len(failures))
        sys.exit(1)
    subprocess.run(["rm", "-rf", tmp])
    print("all ok")


if __name__ == "__main__":
    main()
