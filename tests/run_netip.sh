#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# run_netip.sh VIGILD CLIP [badpass|wide|title|notitle]
# Serve CLIP from netip_server.py, record it with vigild, check the result.
#   wide     the I-frame headers say 2560x1440, as an XM 4 MP camera's do
#            (codec byte 0x13 for H.265): still recorded
#   title    the camera names the channel (ChannelTitle): vigild takes it
#   notitle  the camera never answers ChannelTitle: recorded all the same
set -u
VIGILD=$1 CLIP=$2 MODE=${3:-ok}
HERE=$(cd "$(dirname "$0")" && pwd)
PORT=$((20000 + $$ % 20000))
# In the build tree, not /tmp: recordings need a disk (tmpfs is refused),
# and allow_rootfs lets the build tree be on the root filesystem.
TMP=$(mktemp -d "$PWD/vigil-test.XXXXXX")
printf 'record:\n  allow_rootfs: true\n  chunk_mb: 4\n  pool_mb: 16\nnetip_server:\n  enabled: false\n' > "$TMP/vigil.yaml"
trap 'kill $SRV 2>/dev/null; rm -rf "$TMP"' EXIT

ARGS=
case "$MODE" in
wide) ARGS="--width 2560 --height 1440" ;;
title) ARGS="--title-objects --title Front_door" ;;
notitle) ARGS="--no-title" ;;
esac
# shellcheck disable=SC2086
python3 "$HERE/netip_server.py" "$CLIP" --port $PORT --password secret $ARGS >"$TMP/server.log" 2>&1 &
SRV=$!
for i in 1 2 3 4 5 6 7 8 9 10; do grep -q netip_server "$TMP/server.log" && break; sleep 0.3; done

PASS=secret
[ "$MODE" = badpass ] && PASS=wrong
"$VIGILD" -t 6 -f "$TMP/vigil.yaml" -S 60 -o "$TMP/rec" -c "netip://admin:$PASS@127.0.0.1:$PORT/" >"$TMP/vigild.log" 2>&1
cat "$TMP/vigild.log"

if [ "$MODE" = badpass ]; then
    grep -q "login refused (Ret 203)" "$TMP/vigild.log" || { echo "FAIL: no login error"; exit 1; }
    python3 "$HERE/rec_check.py" "$TMP/rec" --none || { echo "FAIL: recorded with a bad password"; exit 1; }
    echo "PASS: bad password refused"
    exit 0
fi

if [ "$MODE" = title ]; then
    grep -q 'CAM1: the camera calls it "Front_door"' "$TMP/vigild.log" || { echo "FAIL: no title"; exit 1; }
fi
if [ "$MODE" = wide ]; then
    grep -q "I-frame, codec byte 0x1[23] " "$TMP/vigild.log" || { echo "FAIL: no wide I-frame header"; exit 1; }
fi

case "$CLIP" in *.h264) want=h264 ;; *) want=hevc ;; esac
python3 "$HERE/rec_check.py" "$TMP/rec" --codec $want || exit 1
echo PASS
