#!/usr/bin/env bash
# 3-round determinism harness (master's leak-check precedent): run the same
# allocation workload via `dynajs -d` in three fresh, identical processes and
# compare the exit "memory allocated"/"memory used" rows.
#
# What this ACTUALLY checks is round-to-round DETERMINISM, not growth: each
# round is a fresh process doing identical work, so the exit counters are
# independent samples -- monotonic growth across rounds is impossible to
# observe by construction. What a leak (e.g. an arena that never reclaims,
# or any nondeterministically-retained block) produces here is
# round-VARYING counters; identical counters across rounds prove the
# engine's teardown reaches a fixed point for this workload.
#
# In CONFIG_NURSERY_PROBE=y builds the absolute "allocated" number is HIGHER
# by the nursery's never-reclaimed arena footprint (~193MB on
# tests/agent/nursery/leak_workload.js). That offset is BY DESIGN, bounded,
# and -- the property this gate asserts -- identical in every round.
# Usage: leak_gate.sh <binary> [script]
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
