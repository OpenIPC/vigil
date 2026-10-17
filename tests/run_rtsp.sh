#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# run_rtsp.sh VIGILD CODEC:AUTH   (AUTH: none | basic | digest | wrong)
# Record rtsp_server.py's stream with vigild and check it. Exit 77 = skipped
# (no GStreamer RTSP server bindings / encoder).
set -u
VIGILD=$1 CODEC=${2%%:*} AUTH=${2##*:}
HERE=$(cd "$(dirname "$0")" && pwd)
PORT=$((20000 + $$ % 20000))
TMP=$(mktemp -d)
trap 'kill $SRV 2>/dev/null; rm -rf "$TMP"' EXIT

SAUTH=$AUTH
[ "$AUTH" = wrong ] && SAUTH=digest
python3 "$HERE/rtsp_server.py" --port $PORT --codec "$CODEC" --auth "$SAUTH" >"$TMP/server.log" 2>&1 &
SRV=$!
for i in $(seq 1 30); do
    grep -q rtsp_server "$TMP/server.log" && break
    if ! kill -0 $SRV 2>/dev/null; then wait $SRV; rc=$?; [ $rc -eq 77 ] && exit 77; cat "$TMP/server.log"; exit 1; fi
    sleep 0.2
done

CRED=root:123456@
[ "$AUTH" = none ] && CRED=
[ "$AUTH" = wrong ] && CRED=root:nope@
"$VIGILD" -t 6 -o "$TMP/rec" -c "rtsp://${CRED}127.0.0.1:$PORT/test" >"$TMP/vigild.log" 2>&1
cat "$TMP/vigild.log"

if [ "$AUTH" = wrong ]; then
    grep -q "DESCRIBE: 401" "$TMP/vigild.log" && echo "PASS: 401" && exit 0
    echo "FAIL: no 401"; exit 1
fi
FILE=$(ls "$TMP"/rec/CAM1_* 2>/dev/null | head -1)
[ -n "$FILE" ] || { echo "FAIL: nothing recorded"; exit 1; }
if command -v ffprobe >/dev/null; then
    out=$(ffprobe -v error -count_frames -show_entries stream=codec_name,nb_read_frames -of csv=p=0 "$FILE")
    errs=$(ffmpeg -v error -i "$FILE" -f null - 2>&1 | wc -l)
    echo "recorded: $FILE -> $out, decode errors $errs"
    frames=${out##*,}
    [ "$frames" -ge 50 ] && [ "$errs" -eq 0 ] || { echo FAIL; exit 1; }
fi
echo PASS
