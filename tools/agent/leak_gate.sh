#!/usr/bin/env bash
cd "$(dirname "$0")/../.." || exit 2
BIN="$1"
SCRIPT="${2:-tests/agent/nursery/leak_workload.js}"
[ -x "$BIN" ] || { echo "FAIL: $BIN missing or not executable"; exit 2; }
[ -f "$SCRIPT" ] || { echo "FAIL: workload $SCRIPT missing"; exit 2; }
prev=""
rc=0
for r in 1 2 3; do
  line=$(timeout 300 "$BIN" -d "$SCRIPT" 2>&1 | awk '/^memory allocated/{a=$4} /^memory used/{u=$4} END{print "allocated=" a " used=" u}')
  echo "round$r $line"
  if [ -z "$line" ] || [ "$line" = "allocated=  used=" ]; then
    echo "FAIL: no counters captured (did -d print them?)"
    rc=1
  elif [ -n "$prev" ] && [ "$prev" != "$line" ]; then
    echo "FAIL: rounds disagree (nondeterministic leak?)"
    rc=1
  fi
  prev="$line"
done
[ $rc -eq 0 ] && echo "COUNTERS: DETERMINISTIC across 3 rounds"
exit $rc
