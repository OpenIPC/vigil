#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Screenshots of a search on the Channels screen filling as devices answer:
# the real discovery and bridge (test_ui --live-search) against 33 fake
# devices (fake_discovery.py) that answer between 0.2 s and 3 s late, one
# of them over NETIP first and ONVIF later (one row, its protocols grow).
#
#   tests/live_search_shots.sh build/tests/test_ui OUTDIR
#
# Everything stays on 127.0.0.1: the fake answers on loopback for devices
# with made-up addresses, and the search asks only it, by unicast.
set -eu
test_ui=$1
out=$2
here=$(dirname "$0")
mkdir -p "$out"
ports=$(python3 -c '
import socket
s = [socket.socket(socket.AF_INET, socket.SOCK_DGRAM) for _ in range(3)]
for x in s: x.bind(("127.0.0.1", 0))
print(",".join(str(x.getsockname()[1]) for x in s))')

set -- --shared 127.0.0.1 --ports "$ports" \
    --device netip,onvif@10.20.0.31 --delay netip@10.20.0.31=0.2 --delay onvif@10.20.0.31=1.5 \
    --device dhip@10.20.0.32 --delay 10.20.0.32=1.0 \
    --device onvif@10.20.0.33 --delay 10.20.0.33=2.5
i=0
while [ $i -lt 30 ]; do
    case $((i % 3)) in 0) p=netip ;; 1) p=onvif ;; *) p=dhip ;; esac
    set -- "$@" --device "$p@10.20.0.$((40 + i))" --delay "10.20.0.$((40 + i))=$(awk "BEGIN{print 0.3 + $i * 0.09}")"
    i=$((i + 1))
done
python3 "$here/fake_discovery.py" "$@" > "$out/fake.log" 2>&1 &
fake=$!
trap 'kill $fake 2>/dev/null; wait $fake 2>/dev/null || true' EXIT INT TERM
sleep 1
"$test_ui" --live-search "$out" "$ports"
