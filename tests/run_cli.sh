#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# run_cli.sh VIGILD — command-line values that do not fit are refused, not
# shortened: a shortened URL or path can still be valid and name another
# camera, directory or configuration file.
set -u
VIGILD=$1
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
long=$(printf '%0600d' 0)
fail=0

expect_refused() {
    what=$1; shift
    "$VIGILD" -t 1 "$@" >"$TMP/log" 2>&1
    rc=$?
    cat "$TMP/log"
    if [ $rc -ne 2 ] || ! grep -q "too long" "$TMP/log"; then
        echo "FAIL: $what: rc $rc"
        fail=1
    elif grep -q topsecret "$TMP/log"; then
        echo "FAIL: $what: the password is in the log"
        fail=1
    else
        echo "ok: $what refused"
    fi
}

expect_refused "-c URL over 511 bytes" -f "$TMP/none.yaml" -o none -c "rtsp://u:topsecret@127.0.0.1/$long"
expect_refused "-o path over 191 bytes" -f "$TMP/none.yaml" -o "/tmp/$long"
expect_refused "-f path over 255 bytes" -f "/tmp/$long.yaml" -o none
[ $fail -eq 0 ] && echo PASS
exit $fail
