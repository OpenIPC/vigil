#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# run_netip.sh VIGILD CLIP [badpass|wide|title|notitle|alarms|noalarms]
# Serve CLIP from netip_server.py, record it with vigild, check the result.
#   wide     the I-frame headers say 2560x1440, as an XM 4 MP camera's do
#            (codec byte 0x13 for H.265): still recorded
#   title    the camera names the channel (ChannelTitle): vigild takes it
#   notitle  the camera never answers ChannelTitle: recorded all the same
#   alarms   the camera pushes AlarmInfo after AlarmSet and drops the
#            connection once: vigild reports them as camera events, ends
#            what was going on when the connection went, asks again on the
#            new one, and ignores another channel's alarms
#   noalarms the camera refuses AlarmSet: said once, recorded all the same
set -u
VIGILD=$1 CLIP=$2 MODE=${3:-ok}
HERE=$(cd "$(dirname "$0")" && pwd)
PORT=$((20000 + $$ % 20000))
# In the build tree, not /tmp: recordings need a disk (tmpfs is refused),
# and allow_rootfs lets the build tree be on the root filesystem.
TMP=$(mktemp -d "$PWD/vigil-test.XXXXXX")
printf 'record:\n  allow_rootfs: true\nnetip_server:\n  enabled: false\n' > "$TMP/vigil.yaml"
trap 'kill $SRV 2>/dev/null; rm -rf "$TMP"' EXIT

ARGS=
case "$MODE" in
wide) ARGS="--width 2560 --height 1440" ;;
title) ARGS="--title-objects --title Front_door" ;;
notitle) ARGS="--no-title" ;;
alarms) ARGS="--drop-after 4 --alarm-script 0.5:VideoMotion:Start,1:HumanDetect:Start,1.5:VideoMotion:Stop,2:PeaDetect:-,2.2:VideoBlind:Start:1" ;;
noalarms) ARGS="--alarm-refuse" ;;
esac
SECS=6
[ "$MODE" = alarms ] && SECS=10
# shellcheck disable=SC2086
python3 "$HERE/netip_server.py" "$CLIP" --port $PORT --password secret $ARGS >"$TMP/server.log" 2>&1 &
SRV=$!
for i in 1 2 3 4 5 6 7 8 9 10; do grep -q netip_server "$TMP/server.log" && break; sleep 0.3; done

PASS=secret
[ "$MODE" = badpass ] && PASS=wrong
"$VIGILD" -t $SECS -f "$TMP/vigil.yaml" -S 60 -o "$TMP/rec" -c "netip://admin:$PASS@127.0.0.1:$PORT/" >"$TMP/vigild.log" 2>&1
cat "$TMP/vigild.log"

if [ "$MODE" = badpass ]; then
    grep -q "login refused (Ret 203)" "$TMP/vigild.log" || { echo "FAIL: no login error"; exit 1; }
    [ -z "$(ls "$TMP/rec" 2>/dev/null)" ] || { echo "FAIL: recorded with a bad password"; exit 1; }
    echo "PASS: bad password refused"
    exit 0
fi

if [ "$MODE" = title ]; then
    grep -q 'CAM1: the camera calls it "Front_door"' "$TMP/vigild.log" || { echo "FAIL: no title"; exit 1; }
fi
if [ "$MODE" = alarms ]; then
    cat "$TMP/server.log"
    n=$(grep -c "alarm subscription" "$TMP/server.log")
    [ "$n" -ge 2 ] || { echo "FAIL: alarms asked for $n time(s), not again after the reconnect"; exit 1; }
    n=$(grep -c "CAM1: motion start (netip: VideoMotion)" "$TMP/vigild.log")
    [ "$n" -ge 2 ] || { echo "FAIL: motion start seen $n time(s)"; exit 1; }
    grep -q "CAM1: motion stop (netip: VideoMotion)" "$TMP/vigild.log" || { echo "FAIL: no motion stop"; exit 1; }
    grep -q "CAM1: human start (netip: HumanDetect)" "$TMP/vigild.log" || { echo "FAIL: no human start"; exit 1; }
    # No Stop for it: the dropped connection ends it.
    grep -q "CAM1: human stop (netip: HumanDetect)" "$TMP/vigild.log" || { echo "FAIL: human not ended"; exit 1; }
    grep -q 'NETIP alarm "PeaDetect": reported as "other"' "$TMP/vigild.log" &&
        grep -q "CAM1: other start (netip: PeaDetect)" "$TMP/vigild.log" || { echo "FAIL: unknown alarm"; exit 1; }
    ! grep -q "tamper" "$TMP/vigild.log" || { echo "FAIL: another channel's alarm taken"; exit 1; }
fi
if [ "$MODE" = noalarms ]; then
    n=$(grep -c "refuses to send its alarms over NETIP (AlarmSet: Ret 103)" "$TMP/vigild.log")
    [ "$n" -eq 1 ] || { echo "FAIL: the refusal said $n times"; exit 1; }
fi
if [ "$MODE" = wide ]; then
    grep -q "I-frame, codec byte 0x1[23] " "$TMP/vigild.log" || { echo "FAIL: no wide I-frame header"; exit 1; }
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
