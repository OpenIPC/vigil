#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# run_netip.sh VIGILD CLIP [badpass]
# Serve CLIP from netip_server.py, record it with vigild, check the result.
set -u
VIGILD=$1 CLIP=$2 MODE=${3:-ok}
HERE=$(cd "$(dirname "$0")" && pwd)
PORT=$((20000 + $$ % 20000))
TMP=$(mktemp -d)
trap 'kill $SRV 2>/dev/null; rm -rf "$TMP"' EXIT

python3 "$HERE/netip_server.py" "$CLIP" --port $PORT --password secret >"$TMP/server.log" 2>&1 &
SRV=$!
for i in 1 2 3 4 5 6 7 8 9 10; do grep -q netip_server "$TMP/server.log" && break; sleep 0.3; done

PASS=secret
[ "$MODE" = badpass ] && PASS=wrong
"$VIGILD" -t 6 -S 60 -o "$TMP/rec" -c "netip://admin:$PASS@127.0.0.1:$PORT/" >"$TMP/vigild.log" 2>&1
cat "$TMP/vigild.log"

if [ "$MODE" = badpass ]; then
    grep -q "login refused (Ret 203)" "$TMP/vigild.log" || { echo "FAIL: no login error"; exit 1; }
    [ -z "$(ls "$TMP/rec" 2>/dev/null)" ] || { echo "FAIL: recorded with a bad password"; exit 1; }
    echo "PASS: bad password refused"
    exit 0
fi

FILE=$(ls "$TMP"/rec/CAM1_* 2>/dev/null | head -1)
[ -n "$FILE" ] || { echo "FAIL: nothing recorded"; exit 1; }
case "$CLIP" in *.h264) want=h264 ;; *) want=hevc ;; esac
case "$FILE" in *.h264) [ $want = h264 ] ;; *.h265) [ $want = hevc ] ;; *) false ;; esac ||
    { echo "FAIL: wrong extension $FILE"; exit 1; }
if command -v ffprobe >/dev/null; then
    out=$(ffprobe -v error -count_frames -show_entries stream=codec_name,nb_read_frames -of csv=p=0 "$FILE")
    errs=$(ffmpeg -v error -i "$FILE" -f null - 2>&1 | wc -l)
    echo "recorded: $FILE -> $out, decode errors $errs"
    codec=${out%%,*} frames=${out##*,}
    [ "$codec" = $want ] && [ "$frames" -ge 50 ] && [ "$errs" -eq 0 ] || { echo "FAIL"; exit 1; }
fi
echo PASS
