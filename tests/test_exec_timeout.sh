#!/usr/bin/env bash
# test_exec_timeout.sh -- T4: --timeout-ms kills a spin loop (S2)
set -u
cd "$(dirname "$0")/.."
start=$(date +%s%N 2>/dev/null || date +%s)
out=$(./dynajs --timeout-ms 800 -e 'for(;;){}' 2>&1)
rc=$?
end=$(date +%s%N 2>/dev/null || date +%s)
if [ -n "$start" ] && [ ${#start} -gt 10 ]; then el=$(( (end - start) / 1000000 )); else el=0; fi
if [ $rc -ne 0 ] && echo "$out" | grep -qi "interrupt"; then
  echo "test_exec_timeout: ok (rc=$rc, ${el}ms, msg: $(echo "$out" | head -1 | cut -c1-60))"
  exit 0
fi
echo "test_exec_timeout: FAIL rc=$rc elapsed=${el}ms out=$out" >&2
exit 1
