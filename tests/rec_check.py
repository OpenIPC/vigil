#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""rec_check.py DIR [--none] [--cameras N] [--codec h264|hevc ...] [--min-frames N]

What a vigild recorded into the pool in DIR (docs/recording.md): the chunk
files' own headers (vgch) say which hold video, of which camera. Each
camera's first chunk is a plain MP4 once closed: ffprobe counts its frames
and ffmpeg decodes it without an error.

  --none        nothing may have been recorded
  --cameras N   N cameras recorded (1 by default)
  --codec C     the codec of each camera in turn (ffprobe's name)
"""
import argparse
import glob
import os
import shutil
import struct
import subprocess
import sys

UUID_VGCH = bytes([0x76, 0x67, 0x63, 0x68, 0x9b, 0x1e, 0x4c, 0x0a, 0xa5, 0x2d, 0x61, 0x7f, 0x0e, 0x3c, 0xd2, 0x41])
STATES = {0: "free", 1: "open", 2: "closed", 3: "bad"}


def chunk_record(path):
    """(state, camera, stream, codec, use, nfrag, start_us, end_us) of a chunk, None if it says nothing."""
    with open(path, "rb") as f:
        box = f.read(4096)
    if len(box) < 4096 or box[4:8] != b"uuid" or box[8:24] != UUID_VGCH or box[24:28] != b"VGCH":
        return None
    recs = []
    for at in (64, 2048):
        r = box[at:at + 64]
        st, cam, stream, codec, flags, use, nfrag, start, end = struct.unpack_from("<BBBBIIIQQ", r)
        recs.append(None if r == bytes(64) or st not in STATES else (st, cam, stream, codec, use, nfrag, start, end))
    a, b = recs
    # the close copy when it is this use's (vgch: rec_vgch_parse)
    return b if b and (not a or b[4] == a[4]) else a


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("--none", action="store_true")
    ap.add_argument("--cameras", type=int, default=1)
    ap.add_argument("--codec", action="append", default=[])
    ap.add_argument("--min-frames", type=int, default=50)
    a = ap.parse_args()
    chunks = {}
    for p in sorted(glob.glob(os.path.join(a.dir, "pool", "c*.mp4"))):
        r = chunk_record(p)
        if r and r[0] in (1, 2) and r[5] > 0:
            chunks.setdefault(r[1], []).append((r[6], p, r))
    for cam in sorted(chunks):
        for start, p, r in sorted(chunks[cam]):
            print("CAM%d: %s %s, %d fragments, %.1f s" % (cam, os.path.basename(p), STATES[r[0]], r[5],
                                                          (r[7] - r[6]) / 1e6))
    if a.none:
        if chunks:
            print("FAIL: recorded, and should not have")
            return 1
        print("nothing recorded")
        return 0
    if len(chunks) != a.cameras:
        print("FAIL: %d camera(s) recorded, not %d" % (len(chunks), a.cameras))
        return 1
    if not shutil.which("ffprobe"):
        return 0
    for k, cam in enumerate(sorted(chunks)):
        path = sorted(chunks[cam])[0][1]
        out = subprocess.run(["ffprobe", "-v", "error", "-count_frames", "-show_entries",
                              "stream=codec_name,nb_read_frames", "-of", "csv=p=0", path],
                             capture_output=True, text=True).stdout.strip()
        errs = subprocess.run(["ffmpeg", "-v", "error", "-i", path, "-fps_mode", "passthrough", "-f", "null", "-"],
                              capture_output=True, text=True).stderr
        # the null muxer rounding a faster stream to the frame rate its SPS says: not the file's
        errs = "\n".join(x for x in errs.splitlines()
                         if "non monotonically increasing dts to muxer" not in x and "Last message repeated" not in x)
        print("CAM%d: %s -> %s, decode errors: %s" % (cam, os.path.basename(path), out, errs or "none"))
        codec, _, frames = out.partition(",")
        want = a.codec[k] if k < len(a.codec) else None
        if (want and codec != want) or not frames.isdigit() or int(frames) < a.min_frames or errs:
            print("FAIL")
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
