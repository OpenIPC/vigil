#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""rec_test.py VIGILD DATA_DIR [SECONDS]

The recording store end to end (docs/recording.md): two fake NETIP cameras
recorded for about two minutes (SECONDS, 120 by default) into a pool of
4 MiB chunks small enough for the ring to go round; one camera gone for a
while (a gap); a motion event through the API; vigild killed with SIGKILL
in the middle of writing and started again (the recovery); then the
questions -- days, spans, events, status -- and exports across chunks and
the gaps, checked with ffprobe for their frames, duration and start, and
the MSE pieces (init, then fragments) decoded with ffmpeg.
"""
import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

FAILS = 0


def check(cond, what):
    global FAILS
    print("%s %s" % ("ok  " if cond else "FAIL", what), flush=True)
    if not cond:
        FAILS += 1
    return cond


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


class Api:
    def __init__(self, port):
        self.base = "http://127.0.0.1:%d" % port

    def call(self, method, path, body=None):
        data = json.dumps(body).encode() if body is not None else None
        req = urllib.request.Request(self.base + path, data=data, method=method)
        if data:
            req.add_header("Content-Type", "application/json")
        try:
            with urllib.request.urlopen(req, timeout=30) as r:
                return r.status, dict(r.headers), r.read()
        except urllib.error.HTTPError as e:
            return e.code, dict(e.headers), e.read()
        except OSError:
            return 0, {}, b""

    def json(self, method, path, body=None):
        st, h, raw = self.call(method, path, body)
        try:
            return st, json.loads(raw) if raw else None
        except ValueError:
            return st, None


def wait_for(fn, secs, step=0.3):
    end = time.time() + secs
    while time.time() < end:
        v = fn()
        if v:
            return v
        time.sleep(step)
    return fn()


def ffprobe(path, entries):
    r = subprocess.run(["ffprobe", "-v", "error", "-count_frames", "-select_streams", "v:0", "-show_entries", entries,
                        "-of", "json", path], capture_output=True, text=True)
    try:
        return json.loads(r.stdout)
    except ValueError:
        return {}


def decode_errors(path):
    """What ffmpeg says decoding the file, and decode times that do not go forward."""
    r = subprocess.run(["ffmpeg", "-v", "error", "-i", path, "-fps_mode", "passthrough", "-f", "null", "-"],
                       capture_output=True, text=True)
    # The null muxer's own complaint: CAM2 plays a 25 fps clip at 100 fps,
    # its timestamps rounded to the clip's frame rate there. Not the file's.
    errs = "\n".join(line for line in r.stderr.splitlines()
                     if "non monotonically increasing dts to muxer" not in line and "Last message repeated" not in line)
    p = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries", "packet=dts",
                        "-of", "csv=p=0", path], capture_output=True, text=True).stdout.split()
    dts = [int(x) for x in p if x.lstrip("-").isdigit()]
    back = sum(1 for a, b in zip(dts, dts[1:]) if b <= a)
    if back:
        errs += " %d decode times not going forward" % back
    return errs.strip()


def main():
    vigild, data = sys.argv[1], sys.argv[2]
    secs = int(sys.argv[3]) if len(sys.argv) > 3 else 120
    here = os.path.dirname(os.path.abspath(__file__))
    if not shutil.which("ffprobe"):
        print("skip: no ffprobe")
        return 77
    # In the build tree: recordings need a disk, allow_rootfs the root filesystem.
    tmp = tempfile.mkdtemp(prefix="vigil-rec-", dir=os.getcwd())
    procs = []
    try:
        return run(vigild, data, secs, here, tmp, procs)
    finally:
        for p in procs:
            if p.poll() is None:
                p.kill()
                p.wait()
        if FAILS == 0 and not os.environ.get("KEEP"):
            shutil.rmtree(tmp, ignore_errors=True)
        else:
            print("kept", tmp)


def run(vigild, data, secs, here, tmp, procs):
    api_port, c1, c2 = free_port(), free_port(), free_port()
    rec = os.path.join(tmp, "rec")
    cfg = os.path.join(tmp, "vigil.yaml")
    with open(cfg, "w") as f:
        f.write("cameras:\n"
                "  - url: \"netip://admin:@127.0.0.1:%d/\"\n    name: Gate\n"
                "  - url: \"netip://admin:@127.0.0.1:%d/\"\n    name: Yard\n"
                "record:\n  path: %s\n  allow_rootfs: true\n  chunk_mb: 4\n  pool_mb: 16\n  flush_s: 1\n"
                "api:\n  listen: \"127.0.0.1:%d\"\n  auth: false\n"
                "web:\n  enabled: false\nrtsp_server:\n  enabled: false\nonvif_server:\n  enabled: false\n"
                "netip_server:\n  enabled: false\n" % (c1, c2, json.dumps(rec), api_port))

    def camera(port, clip, fps=25):
        p = subprocess.Popen([sys.executable, os.path.join(here, "netip_server.py"), os.path.join(data, clip),
                              "--port", str(port), "--fps", str(fps)], stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL)
        procs.append(p)
        return p

    log = open(os.path.join(tmp, "vigild.log"), "a")

    def start_vigild():
        p = subprocess.Popen([vigild, "-f", cfg], stdout=log, stderr=subprocess.STDOUT)
        procs.append(p)
        return p

    # CAM2 at four times the rate, so that the ring goes round within the
    # first minute and a half; CAM1 a little later and slower, its first
    # chunk the oldest only once it is closed.
    cam2 = camera(c2, "majestic_1080p.h265", 100)
    time.sleep(0.5)
    t_start = time.time()
    v = start_vigild()
    api = Api(api_port)
    time.sleep(3)
    cam1 = camera(c1, "testsrc_640x360.h264")

    def recording(n):
        def f():
            st, d = api.json("GET", "/api/v1/status")
            return st == 200 and sum(1 for c in d["channels"][:n] if c["recording"]) == n
        return f
    check(wait_for(recording(2), 20), "both cameras recording")
    st, s0 = api.json("GET", "/api/v1/record/status")
    check(st == 200 and s0["state"] == "ready" and s0["pool"]["chunk_mb"] == 4 and s0["pool"]["wanted"] == 4,
          "record/status: ready, 4 chunks of 4 MiB (%s)" % (s0 and s0.get("pool")))

    # CAM1 goes away for a while: a gap in its recording.
    time.sleep(max(0, t_start + secs * 0.3 - time.time()))
    cam1.kill()
    cam1.wait()
    t_gap = time.time()
    time.sleep(12)
    cam1 = camera(c1, "testsrc_640x360.h264")
    t_back = time.time()
    check(wait_for(recording(2), 25), "CAM1 back and recording")
    time.sleep(3)
    st, ev = api.json("POST", "/api/v1/channels/1/events", {"events": ["motion"]})
    t_motion = time.time()
    check(st == 200, "a motion event for CAM1 (%s)" % st)
    time.sleep(max(0, t_start + secs * 0.75 - time.time()))
    t_now = time.time()

    st, s1 = api.json("GET", "/api/v1/record/status")
    check(st == 200 and s1["pool"]["open"] == 2 and s1["pool"]["free"] == 0 and s1["pool"]["bad"] == 0,
          "status: every chunk used, two open (%s)" % s1.get("pool"))
    cams = {c["camera"]: c for c in s1.get("cameras", [])}
    check(1 in cams and 2 in cams, "status: both cameras have video")
    # 16 MiB at ~1.7 Mbit/s: the ring went round; CAM2's oldest video is not its first.
    check(cams.get(2, {}).get("oldest", 0) > t_start + 10, "the ring went round: CAM2's oldest video %.0f s after "
          "the start" % (cams.get(2, {}).get("oldest", 0) - t_start))
    check(s1.get("days_kept") is not None and s1.get("write_kbps", 0) > 500,
          "status: write rate %s kbit/s, %s days kept" % (s1.get("write_kbps"), s1.get("days_kept")))

    lt = time.localtime(t_start)
    st, d = api.json("GET", "/api/v1/record/days?camera=1&month=%04d-%02d" % (lt.tm_year, lt.tm_mon))
    check(st == 200 and lt.tm_mday in d["days"], "days: today (%s)" % d)
    st, d = api.json("GET", "/api/v1/record/days?camera=1&month=1999-01")
    check(st == 200 and d["days"] == [], "days: none in 1999")

    st, sp = api.json("GET", "/api/v1/record/spans?camera=1&from=%.3f&to=%.3f" % (t_start - 60, t_now + 60))
    spans = sp["spans"] if st == 200 else []
    print("  CAM1 spans:", [(round(s["from"] - t_start, 1), round(s["to"] - t_start, 1)) for s in spans])
    check(len(spans) == 2, "spans: CAM1 in 2 pieces around its absence (%d)" % len(spans))
    check(spans and t_gap - 6 < spans[0]["to"] <= t_gap + 1,
          "a span ends when the camera went (%.1f s before)" % (t_gap - spans[0]["to"] if spans else -1))
    check(len(spans) > 1 and t_back - 1 < spans[1]["from"] < t_back + 10, "the next one starts when it came back")
    motion = [e for s in spans for e in s["events"] if e["type"] == "motion"]
    check(len(motion) == 1 and abs(motion[0]["time"] - t_motion) < 2,
          "the motion event is in its span (%s)" % [round(e["time"] - t_start, 1) for e in motion])
    st, ev = api.json("GET", "/api/v1/record/events?from=%.3f&to=%.3f" % (t_start - 60, t_now + 60))
    check(st == 200 and len(ev["events"]) == 1 and ev["events"][0]["camera"] == 1 and
          ev["events"][0]["recorded"] and ev["events"][0]["duration"] >= 5,
          "events: CAM1's motion, recorded, %s s" % (ev["events"][0]["duration"] if st == 200 and ev["events"] else "?"))
    st, ev = api.json("GET", "/api/v1/record/events?camera=2&from=%.3f&to=%.3f" % (t_start - 60, t_now + 60))
    check(st == 200 and ev["events"] == [], "events: none for CAM2")

    # ---- export across the gap and the chunks
    first = spans[0]["from"] if spans else t_start
    frm, to = first + 7.25, (spans[-1]["to"] if spans else t_now) - 2
    st, h, body = api.call("GET", "/api/v1/record/export.mp4?camera=1&from=%.3f&to=%.3f" % (frm, to))
    out = os.path.join(tmp, "keep.mp4")
    with open(out, "wb") as f:
        f.write(body)
    check(st == 200 and h.get("Content-Type") == "video/mp4" and int(h.get("Content-Length", 0)) == len(body) and
          "CAM1_" in h.get("Content-Disposition", ""), "export.mp4: %d bytes, %s" % (len(body), h.get("X-Vigil-Codecs")))
    st, c1s = api.json("GET", "/api/v1/record/status")
    p = ffprobe(out, "stream=codec_name,nb_read_frames,start_time:format=duration")
    stream = (p.get("streams") or [{}])[0]
    dur = float(p.get("format", {}).get("duration", 0))
    start = float(stream.get("start_time", 0))
    frames = int(stream.get("nb_read_frames", 0))
    errs = decode_errors(out)
    print("  keep: %s frames, duration %.2f s, start %.3f s; range %.2f s" % (frames, dur, start, to - frm))
    check(stream.get("codec_name") == "h264" and not errs, "keep: H.264, decodes without errors %s" % errs[:200])
    # from the keyframe at or before `from` (the edit list starts the picture at `from`), gaps on the clock
    check(-4.5 <= start <= 0 and abs(dur - (to - frm) + start) < 4.5,
          "keep: lasts the range from its keyframe (%.2f s, from %.3f s)" % (dur, start))
    recorded = sum(min(s["to"], to) - max(s["from"], frm) for s in spans if s["to"] > frm and s["from"] < to)
    check(abs(frames / 25.0 - recorded) < 4.5 - start, "keep: %d frames for %.1f s recorded" % (frames, recorded))

    st, h, body = api.call("GET", "/api/v1/record/export.mp4?camera=1&from=%.3f&to=%.3f&gaps=remove" % (frm, to))
    out2 = os.path.join(tmp, "remove.mp4")
    with open(out2, "wb") as f:
        f.write(body)
    p = ffprobe(out2, "stream=nb_read_frames:format=duration")
    dur2 = float(p.get("format", {}).get("duration", 0))
    frames2 = int((p.get("streams") or [{}])[0].get("nb_read_frames", 0))
    print("  remove: %d frames, duration %.2f s; recorded %.2f s" % (frames2, dur2, recorded))
    check(st == 200 and frames2 == frames and abs(dur2 - recorded) < 4.5 and not decode_errors(out2),
          "remove: the same frames, no gaps: %.1f s" % dur2)

    st, d = api.json("GET", "/api/v1/record/export.mp4?camera=1&from=%.3f&to=%.3f&gaps=split" % (frm, to))
    check(st == 200 and len(d["files"]) == 2, "split: one file per span (%d)" % len(d.get("files", [])))
    if st == 200 and d["files"]:
        st, h, body = api.call("GET", d["files"][-1]["url"])
        out3 = os.path.join(tmp, "split.mp4")
        with open(out3, "wb") as f:
            f.write(body)
        check(st == 200 and not decode_errors(out3), "split: the last span's file plays")
    st, _ = api.json("GET", "/api/v1/record/export.mp4?camera=1&from=%.3f&to=%.3f&layout=faststart" % (frm, to))
    check(st == 501, "layout faststart: not yet (%s)" % st)
    st, _ = api.json("GET", "/api/v1/record/export.mp4?camera=3&from=%.3f&to=%.3f" % (frm, to))
    check(st == 404, "a camera with nothing recorded: 404")
    st, _ = api.json("GET", "/api/v1/record/export.mp4?camera=1&from=%.3f&to=%.3f" % (to, frm))
    check(st == 400, "to before from: 400")

    # ---- the MSE pieces: init, then fragments one request after another
    st, sp2 = api.json("GET", "/api/v1/record/spans?camera=2&from=%.3f&to=%.3f" % (t_start - 60, t_now + 60))
    at = sp2["spans"][0]["from"] + 3 if st == 200 and sp2["spans"] else t_now - 30
    st, h, init = api.call("GET", "/api/v1/record/init?camera=2&at=%.3f" % at)
    codecs = h.get("X-Vigil-Codecs", "")
    check(st == 200 and init[4:8] == b"ftyp" and codecs.startswith("hvc1."), "init: CAM2's, %s" % codecs)
    pieces, frm_q, cont, base = [init], at, 0, None
    for _ in range(3):
        q = "/api/v1/record/fragments?camera=2&from=%.3f&count=3&cont=%d" % (frm_q, cont)
        if base:
            q += "&base=" + base
        st, h, b = api.call("GET", q)
        if st != 200:
            break
        pieces.append(b)
        base = h.get("X-Vigil-Base")
        frm_q, cont = float(h.get("X-Vigil-Next", 0)), 1
    mse = os.path.join(tmp, "mse.mp4")
    with open(mse, "wb") as f:
        f.write(b"".join(pieces))
    p = ffprobe(mse, "stream=codec_name,nb_read_frames")
    n = int((p.get("streams") or [{}])[0].get("nb_read_frames", 0))
    check(len(pieces) == 4 and n >= 50 and not decode_errors(mse),
          "init + 3 x 3 fragments: %d frames, one timeline, no decode error" % n)

    # ---- killed while writing: what was flushed must be there after the restart
    time.sleep(max(0, t_start + secs * 0.85 - time.time()))
    v.send_signal(signal.SIGKILL)
    v.wait()
    t_kill = time.time()
    # The cameras off while vigild starts again: nothing is reused before
    # the recovered chunks are looked at.
    for c in (cam1, cam2):
        c.kill()
        c.wait()
    time.sleep(1)
    log.flush()
    pos = os.path.getsize(os.path.join(tmp, "vigild.log"))
    v = start_vigild()

    def ready():
        st, d = api.json("GET", "/api/v1/record/status")
        return st == 200 and d["state"] == "ready"
    check(wait_for(ready, 15, 0.1), "after SIGKILL: started again, the pool ready")
    text = open(os.path.join(tmp, "vigild.log")).read()[pos:]
    recovered = [line for line in text.splitlines() if "recovered:" in line]
    check(len(recovered) == 2, "the two open chunks were recovered: %s" % [line.split("rec: ")[-1] for line in recovered])
    # At once, before the ring reuses anything: flush_s 1, so at most that,
    # a fragment in progress and the kill's own second are lost.
    for cam in (1, 2):
        st, sp = api.json("GET", "/api/v1/record/spans?camera=%d&from=%.3f&to=%.3f" % (cam, t_kill - 60, t_kill + 5))
        spans = sp["spans"] if st == 200 else []
        check(spans and t_kill - 6 < spans[-1]["to"] <= t_kill + 0.5,
              "CAM%d: only the unflushed tail lost at the kill (%.1f s)" %
              (cam, t_kill - spans[-1]["to"] if spans else -1))
    # The recovered chunk plays up to where the journal and the tail scan left it.
    st, h, body = api.call("GET", "/api/v1/record/export.mp4?camera=1&from=%.3f&to=%.3f" % (t_kill - 20, t_kill + 5))
    out4 = os.path.join(tmp, "kill.mp4")
    with open(out4, "wb") as f:
        f.write(body)
    p = ffprobe(out4, "stream=nb_read_frames:format=duration")
    d4 = float(p.get("format", {}).get("duration", 0))
    check(st == 200 and not decode_errors(out4) and 15 < d4 < 26, "the recovered chunk exports and plays: %.1f s" % d4)
    cam2 = camera(c2, "majestic_1080p.h265", 100)
    cam1 = camera(c1, "testsrc_640x360.h264")
    check(wait_for(recording(2), 25), "recording again")
    time.sleep(12)
    t_end = time.time()
    st, s2 = api.json("GET", "/api/v1/record/status")
    check(st == 200 and s2["pool"]["open"] == 2 and s2["pool"]["bad"] == 0, "after the restart: %s" % s2.get("pool"))
    st, sp = api.json("GET", "/api/v1/record/spans?camera=1&from=%.3f&to=%.3f" % (t_kill, t_end))
    check(st == 200 and len(sp["spans"]) == 1 and sp["spans"][0]["to"] > t_end - 6,
          "CAM1 recorded on after the restart (%s)" % (sp or {}).get("spans"))

    # ---- freeing a range
    st, d = api.json("DELETE", "/api/v1/record/recordings?camera=2&from=0&to=%.3f" % (t_start + 30))
    check(st == 200 and "freed" in d, "DELETE recordings: %s" % d)
    st, d = api.json("POST", "/api/v1/record/takeover")
    check(st == 409, "takeover of our own pool: 409")

    # ---- a clean stop leaves every chunk closed, playable as it is
    v.send_signal(signal.SIGTERM)
    v.wait(timeout=20)
    r = subprocess.run([sys.executable, os.path.join(here, "rec_check.py"), rec, "--cameras", "2", "--codec", "h264",
                        "--codec", "hevc"], capture_output=True, text=True)
    print(r.stdout.strip())
    check(r.returncode == 0 and " open," not in r.stdout, "after SIGTERM: every chunk closed, each decodes")
    print("%d failure(s)" % FAILS)
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
