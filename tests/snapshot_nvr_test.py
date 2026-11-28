#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The recorder's own snapshots: test_snapshot (snapshot.c on a faked
decoder picture), then its JPEGs decoded with PIL: the size, the video
range made full (16 -> 0, 235 -> 255) and the colours where they were.

    snapshot_nvr_test.py TEST_SNAPSHOT OUTDIR

Exit 77 (skipped) without PIL, after test_snapshot itself passed.
"""
import os
import subprocess
import sys


def main():
    exe, out = sys.argv[1], sys.argv[2]
    if subprocess.run([exe, out]).returncode != 0:
        return 1
    try:
        from PIL import Image
    except ImportError:
        print("skip: no PIL to decode the JPEGs with")
        return 77
    fails = []

    def check(cond, what):
        print(("ok   " if cond else "FAIL ") + what, flush=True)
        if not cond:
            fails.append(what)

    im = Image.open(os.path.join(out, "nvr.jpg"))
    im.load()
    check(im.format == "JPEG" and im.size == (704, 576), "nvr.jpg: a JPEG PIL decodes, 704x576 (%s)" % (im.size,))
    g = im.convert("L")
    left, right, mid = g.getpixel((2, 500)), g.getpixel((701, 500)), g.getpixel((352, 500))
    check(left <= 6 and right >= 249, "video range made full: black %d at the left, white %d at the right" % (left, right))
    check(110 <= mid <= 145, "grey in the middle (%d)" % mid)
    r, gg, b = im.convert("RGB").getpixel((30, 30))
    check(r > gg + 80 and r > b + 80, "the red block where it was (%d,%d,%d)" % (r, gg, b))
    r, gg, b = im.convert("RGB").getpixel((352, 400))
    check(abs(r - gg) < 12 and abs(gg - b) < 12, "grey elsewhere (%d,%d,%d)" % (r, gg, b))
    full = Image.open(os.path.join(out, "full.jpg"))
    full.load()
    check(full.size == (1920, 1080), "full.jpg: 1920x1080")
    print("%d failed" % len(fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
