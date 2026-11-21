#!/bin/sh
# tools/test-slot.sh [-n N] -- CMD [ARGS...] -- run CMD while holding N test
# slots (default 1) of ONE machine-wide pool, so the number of test processes
# running at once never exceeds DYN_TEST_CAP however many gate stages, runners
# and case checkers overlap.
#
# Pool: DYN_TEST_POOL (default <repo>/.prepush/test-pool), DYN_TEST_CAP slots
# (default: the CPU count). A slot is a directory
# slot.<i> made with mkdir (atomic: exactly one taker wins) holding the
# holder's pid; a slot whose holder is gone is reclaimed, so a kill -9 never
# leaks capacity. N is clamped to the cap. DYN_TEST_SLOT_HELD=1 (set for CMD)
# makes a nested call run CMD directly, so a holder never waits on itself.
# CMD's exit status is returned unchanged.
if [ "${1:-}" = "--selftest" ]; then
  d=$(mktemp -d "${TMPDIR:-/tmp}/testslot.XXXXXX") || exit 2
  self=$0
  i=0; while [ "$i" -lt 12 ]; do
    DYN_TEST_POOL="$d/pool" DYN_TEST_CAP=3 DYN_TEST_SLOT_HELD= "$self" -- sh -c 'mkdir "$1/live.$$"; n=$(ls -d "$1"/live.* | wc -l | tr -d " "); echo "$n" >>"$1/peaks"; sleep 0.4; rmdir "$1/live.$$"' _ "$d" &
    i=$((i + 1)); done
  wait
  peak=$(sort -n "$d/peaks" | tail -1); runs=$(wc -l <"$d/peaks" | tr -d ' ')
  fail=0
  [ "$runs" = 12 ] || { echo "FAIL: test-slot selftest: 12 holders, $runs ran"; fail=1; }
  [ "$peak" -le 3 ] || { echo "FAIL: test-slot selftest: cap 3 but $peak ran at once"; fail=1; }
  [ "$peak" -ge 2 ] || { echo "FAIL: test-slot selftest: cap 3 but never more than $peak at once (no parallelism)"; fail=1; }
  mkdir -p "$d/pool2/slot.0"; echo 999999 >"$d/pool2/slot.0/pid"
  DYN_TEST_POOL="$d/pool2" DYN_TEST_CAP=1 DYN_TEST_SLOT_HELD= "$self" -- true || { echo "FAIL: test-slot selftest: a dead holder's slot was not reclaimed"; fail=1; }
  [ -d "$d/pool2/slot.0" ] && { echo "FAIL: test-slot selftest: slot not released after the command"; fail=1; }
  DYN_TEST_POOL="$d/pool3" DYN_TEST_CAP=2 DYN_TEST_SLOT_HELD= "$self" -- sh -c 'exit 7'; [ $? = 7 ] || { echo "FAIL: test-slot selftest: exit status not passed through"; fail=1; }
  find "$d" -depth -delete 2>/dev/null
  [ "$fail" = 0 ] && echo "test-slot selftest: ok (peak $peak of cap 3 over 12 holders)"
  exit "$fail"
fi
n=1
if [ "${1:-}" = "-n" ]; then n=$2; shift 2; fi
[ "${1:-}" = "--" ] && shift
[ $# -ge 1 ] || { echo "usage: tools/test-slot.sh [-n N] -- CMD [ARGS...]" >&2; exit 2; }
if [ -n "${DYN_TEST_SLOT_HELD:-}" ] || [ "${DYN_TEST_CAP:-x}" = 0 ]; then exec "$@"; fi
root=$(cd "$(dirname "$0")/.." && pwd)
pool=${DYN_TEST_POOL:-$root/.prepush/test-pool}
cap=${DYN_TEST_CAP:-$( (command -v nproc >/dev/null 2>&1 && nproc) || sysctl -n hw.ncpu 2>/dev/null || echo 4 )}
case "$cap" in ''|*[!0-9]*) echo "test-slot: DYN_TEST_CAP must be a positive integer (got '$cap')" >&2; exit 2 ;; esac
case "$n" in ''|*[!0-9]*|0) echo "test-slot: -n must be a positive integer (got '$n')" >&2; exit 2 ;; esac
[ "$n" -gt "$cap" ] && n=$cap
mkdir -p "$pool" 2>/dev/null || { echo "test-slot: cannot create $pool" >&2; exit 2; }
held=""
release() { for s in $held; do rm -f "$s/pid" 2>/dev/null; rmdir "$s" 2>/dev/null; done; held=""; }
child=""
stop() { [ -n "$child" ] && kill -"$1" "$child" 2>/dev/null && wait "$child" 2>/dev/null; release; exit "$2"; }
trap 'release' EXIT
trap 'stop INT 130' INT
trap 'stop TERM 143' TERM
trap 'stop HUP 129' HUP
me=$$
got=0
while [ "$got" -lt "$n" ]; do
  i=0
  while [ "$i" -lt "$cap" ] && [ "$got" -lt "$n" ]; do
    s="$pool/slot.$i"
    if mkdir "$s" 2>/dev/null; then
      echo "$me" >"$s/pid"
      held="$held $s"; got=$((got + 1))
    else
      p=$(cat "$s/pid" 2>/dev/null)
      if { [ -n "$p" ] && ! kill -0 "$p" 2>/dev/null; } ||
         { [ -z "$p" ] && [ -n "$(find "$s" -maxdepth 0 -mmin +1 2>/dev/null)" ]; }; then
        rm -f "$s/pid" 2>/dev/null; rmdir "$s" 2>/dev/null
        continue
      fi
    fi
    i=$((i + 1))
  done
  [ "$got" -lt "$n" ] && sleep 0.05
done
exec 3<&0
DYN_TEST_SLOT_HELD=1 "$@" <&3 &
child=$!
wait "$child"
rc=$?
child=""
release
exit "$rc"
