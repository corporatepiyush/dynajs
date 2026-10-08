#!/usr/bin/env bash
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
