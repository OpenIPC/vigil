#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# run_rtsp.sh VIGILD CODEC:AUTH   (AUTH: none | basic | digest | wrong)
# Record rtsp_server.py's stream with vigild and check it. Exit 77 = skipped
# (no GStreamer RTSP server bindings / encoder).
set -u
VIGILD=$1 CODEC=${2%%:*} AUTH=${2##*:}
HERE=$(cd "$(dirname "$0")" && pwd)
PORT=$((20000 + $$ % 20000))
# In the build tree, not /tmp: recordings need a disk (tmpfs is refused),
# and allow_rootfs lets the build tree be on the root filesystem.
TMP=$(mktemp -d "$PWD/vigil-test.XXXXXX")
printf 'record:\n  allow_rootfs: true\n  chunk_mb: 4\n  pool_mb: 16\n' > "$TMP/vigil.yaml"
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
"$VIGILD" -t 6 -f "$TMP/vigil.yaml" -o "$TMP/rec" -c "rtsp://${CRED}127.0.0.1:$PORT/test" >"$TMP/vigild.log" 2>&1
cat "$TMP/vigild.log"

if [ "$AUTH" = wrong ]; then
    grep -q "DESCRIBE: 401" "$TMP/vigild.log" && echo "PASS: 401" && exit 0
    echo "FAIL: no 401"; exit 1
fi
python3 "$HERE/rec_check.py" "$TMP/rec" || exit 1
echo PASS
