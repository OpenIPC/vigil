#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# run_faults.sh VIGILD CLIP CASE — misbehaving cameras must not wedge vigild.
#   chatty     NETIP login never answered, unrelated messages keep coming:
#              open() gives up within its deadline
#   stall      NETIP body arrives in part, then nothing: no busy loop
#   audioonly  NETIP keeps sending audio but no video: the channel reconnects
#   shutdown   RTSP and NETIP servers that accept and never answer: vigild
#              still stops promptly
set -u
VIGILD=$1 CLIP=$2 CASE=$3
HERE=$(cd "$(dirname "$0")" && pwd)
PORT=$((20000 + $$ % 20000))
TMP=$(mktemp -d)
SRV=
trap '[ -n "$SRV" ] && kill $SRV 2>/dev/null; rm -rf "$TMP"' EXIT

start_netip() {
    python3 "$HERE/netip_server.py" "$CLIP" --port $PORT "$@" >"$TMP/server.log" 2>&1 &
    SRV=$!
    for i in $(seq 1 20); do grep -q netip_server "$TMP/server.log" && return; sleep 0.2; done
}

case $CASE in
chatty)
    start_netip --chatty
    "$VIGILD" -t 16 -o none -c "netip://admin:@127.0.0.1:$PORT/" >"$TMP/v.log" 2>&1
    cat "$TMP/v.log"
    grep -q "no reply to msg 1000 within" "$TMP/v.log" || { echo "FAIL: login never gave up"; exit 1; }
    ;;
stall)
    start_netip --stall
    "$VIGILD" -t 30 -o none -c "netip://admin:@127.0.0.1:$PORT/" >"$TMP/v.log" 2>&1 &
    pid=$!
    sleep 4
    read -r -a st < /proc/$pid/stat
    kill $pid; wait $pid
    ticks=$(( st[13] + st[14] )) hz=$(getconf CLK_TCK)
    echo "CPU over 4 s with a stalled body: $ticks ticks (HZ $hz)"
    [ $ticks -lt $(( hz / 2 )) ] || { echo "FAIL: busy loop"; exit 1; }
    ;;
audioonly)
    start_netip --video-frames 50
    "$VIGILD" -t 12 -o none -c "netip://admin:@127.0.0.1:$PORT/" >"$TMP/v.log" 2>&1
    cat "$TMP/v.log"
    grep -q "no data for" "$TMP/v.log" || { echo "FAIL: audio kept a dead video channel alive"; exit 1; }
    ;;
shutdown)
    python3 -c '
import socket, sys, time
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", int(sys.argv[1]))); s.listen(8); print("silent", flush=True)
conns = []
while True:
    c, _ = s.accept(); conns.append(c)
' $PORT >"$TMP/server.log" 2>&1 &
    SRV=$!
    for i in $(seq 1 20); do grep -q silent "$TMP/server.log" && break; sleep 0.2; done
    start=$(date +%s%N)
    "$VIGILD" -t 2 -o none -c "rtsp://127.0.0.1:$PORT/x" -c "netip://admin:@127.0.0.1:$PORT/" >"$TMP/v.log" 2>&1
    ms=$(( ($(date +%s%N) - start) / 1000000 ))
    cat "$TMP/v.log"
    echo "vigild -t 2 against silent cameras took ${ms} ms"
    [ $ms -lt 4500 ] || { echo "FAIL: stop waited for the cameras"; exit 1; }
    ;;
*) echo "unknown case $CASE"; exit 2 ;;
esac
echo PASS
