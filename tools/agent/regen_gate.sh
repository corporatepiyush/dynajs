#!/usr/bin/env bash
# regen gate: run each corpus .js under $1 (binary), byte-compare to its .base
# pin (pin = program output + a trailing "rc=0" line).
# The corpus (22 programs + pins) is copied from /tmp/pip/regen and is NOT
# committed (tools/agent/regen_corpus/ is local-only), so the preflight
# hard-fails loudly when it is absent instead of looping over zero files and
# printing a vacuous "0/0".
# Usage: regen_gate.sh <binary> ; prints "N/M ok", lists mismatches, exit rc.
cd "$(dirname "$0")/../.." || exit 2
BIN="$1"
[ -x "$BIN" ] || { echo "FAIL: no binary $BIN"; exit 2; }
CORPUS=tools/agent/regen_corpus
if [ ! -d "$CORPUS" ] || ! ls "$CORPUS"/*.js >/dev/null 2>&1; then
  echo "FAIL: regen corpus missing at $CORPUS"
  echo "      restore it with: cp -r /tmp/pip/regen $CORPUS"
  exit 2
fi
ok=0; fail=0; total=0
for f in "$CORPUS"/*.js; do
  total=$((total+1))
  n=$(basename "$f" .js)
  pin="$CORPUS/$n.base"
  if [ ! -f "$pin" ]; then
    echo "FAIL $n: missing pin $pin"
    fail=$((fail+1))
    continue
  fi
  out=$(mktemp out_regen.XXXXXX)
  timeout 60 "$BIN" "$f" > "$out" 2>&1
  rc=$?
  if [ $rc -ne 0 ]; then
    echo "rc=$rc" >> "$out"
    echo "RUN-FAIL $n (rc=$rc)"
    fail=$((fail+1))
    rm -f "$out"
    continue
  fi
  echo "rc=0" >> "$out"
  if cmp -s "$out" "$pin"; then
    ok=$((ok+1))
  else
    echo "DIFF $n"
    fail=$((fail+1))
  fi
  rm -f "$out"
done
echo "$ok/$total ok, $fail fail"
[ "$fail" -eq 0 ]
