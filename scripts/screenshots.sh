#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Every screen and menu of the on-screen UI as PNGs, for looking at (not
# the test references in tests/ui):
#
#   scripts/screenshots.sh BUILD_DIR [OUT_DIR]     (default BUILD_DIR/screenshots)
#   cmake --build build --target screenshots
#
# The headless UI (test_ui --shots) with made-up cameras: named, recording,
# one with motion, one connecting, one failing; 16 channels faked for the
# larger layouts. 1280x720, stable names (live-2x2.png, context-split.png,
# menu-p1.png, ...). Python 3 standard library only.
set -e
BUILD=${1:?usage: $0 BUILD_DIR [OUT_DIR]}
OUT=${2:-$BUILD/screenshots}
TOP=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$OUT"
"$BUILD/tests/test_ui" --shots "$OUT" >/dev/null
python3 - "$TOP/tests" "$OUT" <<'PY'
import glob, os, sys
sys.path.insert(0, sys.argv[1])
from ui_compare import read_ppm, write_png
for ppm in sorted(glob.glob(os.path.join(sys.argv[2], "*.ppm"))):
    w, h, rgb = read_ppm(ppm)
    write_png(ppm[:-4] + ".png", w, h, rgb)
    os.remove(ppm)
PY
rm -f "$OUT"/vigil-*.json
ls "$OUT"/*.png | wc -l | sed "s|$| screens in $OUT|"
