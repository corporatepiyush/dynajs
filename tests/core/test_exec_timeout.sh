#!/usr/bin/env bash
set -u
cd "$(dirname "$0")/../.." || exit 1
start=$EPOCHREALTIME
out=$(./dynajs --timeout-ms 800 -e 'for(;;){}' 2>&1)
rc=$?
end=$EPOCHREALTIME
el=$(( (10#${end//[.,]/} - 10#${start//[.,]/}) / 1000 ))
if [ $rc -ne 0 ] && echo "$out" | grep -qi "interrupt" && [ "$el" -ge 700 ] && [ "$el" -lt 10000 ]; then
  echo "test_exec_timeout: ok (rc=$rc, ${el}ms, msg: $(echo "$out" | head -1 | cut -c1-60))"
  exit 0
fi
echo "test_exec_timeout: FAIL rc=$rc elapsed=${el}ms out=$out" >&2
exit 1
