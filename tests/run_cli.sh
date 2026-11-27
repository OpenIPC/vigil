#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# run_cli.sh VIGILD — command-line values that do not fit are refused, not
# shortened: a shortened URL or path can still be valid and name another
# camera, directory or configuration file. And -w, which writes the
# configuration out.
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
# -w: a JSON file converted to YAML, 0600, its password sealed (and the
# JSON file too: it is loaded first); printed with -w -, also under -s
# (where stdout is the syslog pipe: the passwords must not go there).
printf '{"version": 1, "cameras": [{"url": "rtsp://u:topsecret@10.0.0.2/live", "name": "Gate"}]}\n' >"$TMP/old.json"
if "$VIGILD" -f "$TMP/old.json" -w "$TMP/new.yaml" 2>"$TMP/log" &&
   grep -q '^  - url: "rtsp://u:enc:v1:[A-Za-z0-9+/=]*@10.0.0.2/live"$' "$TMP/new.yaml" &&
   ! grep -q topsecret "$TMP/new.yaml" "$TMP/old.json" "$TMP/log" && [ "$(stat -c %a "$TMP/vigil.key")" = 600 ] &&
   grep -q '^    name: Gate$' "$TMP/new.yaml" && [ "$(stat -c %a "$TMP/new.yaml")" = 600 ]; then
    echo "ok: -w converts JSON to YAML"
else
    cat "$TMP/log"; echo "FAIL: -w converts JSON to YAML"; fail=1
fi
unsealed() { sed 's|enc:v1:[A-Za-z0-9+/=]*|enc:v1:|g' "$1"; }
for s in "" -s; do
    if "$VIGILD" $s -f "$TMP/new.yaml" -w - >"$TMP/out" 2>"$TMP/log" &&
       grep -q '^version: 1$' "$TMP/out" && grep -q 'u:enc:v1:' "$TMP/out" &&
       [ "$(unsealed "$TMP/out")" = "$(unsealed "$TMP/new.yaml")" ]; then
        echo "ok: -w - ${s:+with $s }prints the file"
    else
        echo "FAIL: -w - ${s:+with $s }prints the file"; fail=1
    fi
done
printf '{"cameras": [{"url": "capture:0", "name": "Garage \351"}]}\n' >"$TMP/latin1.json"
"$VIGILD" -f "$TMP/latin1.json" -w "$TMP/latin1.yaml" >"$TMP/log" 2>&1
rc=$?
if [ $rc -eq 1 ] && [ ! -e "$TMP/latin1.yaml" ] && grep -q "cameras\[0\].name: not valid UTF-8" "$TMP/log"; then
    echo "ok: -w names a value that is not UTF-8, writes no YAML"
else
    cat "$TMP/log"; echo "FAIL: -w over a Latin-1 name: rc $rc"; fail=1
fi
printf 'record:\n  segment: 1\n' >"$TMP/bad.yaml"
"$VIGILD" -f "$TMP/bad.yaml" -w "$TMP/out.yaml" >"$TMP/log" 2>&1
rc=$?
if [ $rc -eq 1 ] && [ ! -e "$TMP/out.yaml" ] && grep -q "not writing" "$TMP/log"; then
    echo "ok: -w refuses a file that did not load"
else
    cat "$TMP/log"; echo "FAIL: -w over an invalid file: rc $rc"; fail=1
fi

[ $fail -eq 0 ] && echo PASS
exit $fail
