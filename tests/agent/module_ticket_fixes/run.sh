#!/bin/bash
# run.sh -- run the module-ticket probes under dynajs (dyna:* modules only
# load here; goldens are python-decimal baked in at generation time).
# Usage: bash run.sh [path-to-dynajs]   (default: ../../../dynajs)
set -u
cd "$(dirname "$0")"
BIN="${1:-$PWD/../../../dynajs}"
TMO="${TMO:-300}"
rc=0
for p in probes/*.js; do
  out=$(timeout "$TMO" "$BIN" "$p" 2>&1)
  r=$?
  summ=$(echo "$out" | grep -m1 '^SUMMARY ' || echo "SUMMARY none pass=0 fail=0")
  res=$(echo "$out" | grep -m1 '^RESULT ' | awk '{print $2}')
  echo "$p: rc=$r ${res:-NORESULT}  ($summ)"
  echo "$out" | grep -m5 '^FAIL ' || true
  if [ "$r" -ne 0 ] || [ "${res:-FAIL}" != "PASS" ]; then rc=1; fi
done
if [ "$rc" -eq 0 ]; then echo "run.sh: ALL PROBES PASS"; else echo "run.sh: PROBES FAILED"; fi
exit "$rc"
