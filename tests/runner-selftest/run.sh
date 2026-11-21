#!/usr/bin/env bash
# tests/runner-selftest/run.sh -- prove the parallel test runner's VERDICT
# LOGIC, with five stub suites that each break it in one specific way
# (B1-06.5/.6). Until this existed, the runner's verdict rules were asserted
# only by reading the script; nothing executed them.
#
# The five shapes, and the failure each one MUST produce:
#   exits 1               -> FAIL (the obvious case)
#   prints FAIL, exits 0  -> FAIL (the suite counted failures, never propagated)
#   prints nothing        -> NOT a failure. TRIED as a rule and REJECTED: 6 of
#                            45 real core suites print nothing when they pass
#                            (their only failure signal is a throw), so the rule
#                            turned a green stage red. See the comment block in
#                            tools/run-tests-parallel.sh and ticket
#                            B1-06.6-required-marker. This selftest asserts the
#                            decision instead: silence must NOT be reported.
#   spins forever         -> TIMEOUT via the DEFAULT bound (a suite with no
#                            "// timeout:" used to run with no bound at all)
#   blocks on a socket    -> TIMEOUT via the DEFAULT wall bound (0% CPU, so
#                            only a wall bound can catch it)
#
# All five must be reported, and the runner must exit nonzero.
#
# Then the process-group rule: a suite that backgrounds a helper
# (`sh -c 'helper &'` -- the helper is reparented to pid 1 at once) must leave
# NO process behind when it exits normally, when its wall bound kills it, or
# when the runner itself is interrupted. Orphaned mock servers from two suites
# once piled up to ~290 processes alive for up to 19 hours.
#
# Then the slot rule (F-cap-1): a suite whose header says "// slots: N" takes
# N slots of the machine-wide pool (tools/test-slot.sh -n N), so under
# DYN_TEST_CAP=2 three "slots: 2" suites never overlap (peak 1), while under a
# cap of 6 they do (peak >= 2: the declaration is not a global lock), and
# "slots: 9" under a cap of 2 still runs (clamped to the cap).
#
# Usage: tests/runner-selftest/run.sh [path-to-runner]
set -uo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
RUNNER="${1:-$root/tools/run-tests-parallel.sh}"
work=$(mktemp -d "${TMPDIR:-/tmp}/runner-selftest.XXXXXX")
trap 'rm -rf "$work"' EXIT INT TERM

# A stand-in for the engine: the runner only execs "$DYNAJS <flags> <file>", so
# a shell script with that name exercises the runner's verdict logic exactly.
cat > "$work/dyna" <<'EOF'
#!/usr/bin/env bash
# find the suite file: last argument
for a in "$@"; do f="$a"; done
name=$(basename "$f" .js)
case "$name" in
  exits1)      echo "starting"; echo "boom"; exit 1 ;;
  prints_fail) echo "checking 3 things"; echo "  FAIL  thing two is wrong"; exit 0 ;;
  silent)      exit 0 ;;
  spin)        while :; do :; done ;;
  blocks)      exec 3<>/dev/tcp-not-a-real-host/9 2>/dev/null || { sleep 600; } ;;
  ok1)         echo "ok 1"; echo "all checks passed"; exit 0 ;;
  ok2)         echo "ok 2"; exit 0 ;;
  orphan_exit) sh -c "'$(dirname "$f")/helper.sh' >/dev/null 2>&1 &"; echo "helper started"; exit 0 ;;
  orphan_hang) sh -c "'$(dirname "$f")/helper.sh' >/dev/null 2>&1 &"; sleep 600 ;;
  wide*)       d=$(dirname "$f"); mkdir "$d/live.$name"; ls -d "$d"/live.* | wc -l | tr -d ' ' >> "$d/peaks.$PEAKS"; sleep 0.4; rmdir "$d/live.$name"; exit 0 ;;
esac
exit 0
EOF
chmod +x "$work/dyna"

mk(){ printf '// nothing\n' > "$work/$1.js"; }
mk exits1; mk prints_fail; mk silent; mk spin; mk blocks; mk ok1; mk ok2; mk orphan_exit; mk orphan_hang
printf '#!/bin/sh\nwhile :; do sleep 1; done\n' > "$work/helper.sh"; chmod +x "$work/helper.sh"
helpers(){ pgrep -f "$work/helper.sh" | wc -l | tr -d ' '; }

echo "== runner verdict selftest (7 stub suites, 5 of them broken)"
out="$work/out.log"
# DEV_SUITE_TIMEOUT=8 so the two unbounded shapes finish quickly; the point is
# that they are bounded AT ALL, not how long the bound is. The suite-times
# record starts EMPTY: an empty record once made the longest-first reorder drop
# every suite, and the run "passed" zero of them.
: > "$work/times.tsv"
DEV_SUITE_TIMES="$work/times.tsv" DYNAJS="$work/dyna" DEV_JOBS=4 DEV_SUITE_TIMEOUT=8 DEV_SUITE_CPU_TIMEOUT=4 \
  "$RUNNER" "$work/exits1.js" "$work/prints_fail.js" "$work/silent.js" \
            "$work/spin.js" "$work/blocks.js" "$work/ok1.js" "$work/ok2.js" \
  >"$out" 2>&1
rc=$?
sed 's/^/    /' "$out" | grep -vE '^\s*$' | tail -40

fail=0
note(){ printf '  %-6s %s\n' "$1" "$2"; }
[ "$rc" != 0 ] && note ok "runner exited nonzero (rc=$rc)" || { note FAIL "runner exited 0"; fail=1; }
grep -q "FAIL: .*exits1.js (exit 1)" "$out" && note ok "exits 1 -> FAIL" || { note FAIL "exits 1 not reported"; fail=1; }
grep -q "printed FAIL but exited 0" "$out" && note ok "prints FAIL + exit 0 -> FAIL" || { note FAIL "printed-FAIL exit-0 not reported"; fail=1; }
grep -q "silent.js" <(grep '^FAIL' "$out") && { note FAIL "a silent-but-passing suite was reported as failing (the rule that was tried and rejected)"; fail=1; } \
  || note ok "a silent suite is NOT reported as failing (deliberate, see header)"
grep -q "TIMEOUT: .*spin.js" "$out" && note ok "infinite spin -> TIMEOUT (default bound)" || { note FAIL "spin not bounded"; fail=1; }
grep -q "TIMEOUT: .*blocks.js" "$out" && note ok "blocked syscall -> TIMEOUT (default wall bound)" || { note FAIL "blocked suite not bounded"; fail=1; }
grep -q "all 7 test suites passed" "$out" && { note FAIL "runner claimed a pass"; fail=1; } \
  || note ok "runner did not claim a pass"
grep -q "FAIL: 4 of 7 test suites failed" "$out" && note ok "exactly the 4 reported-broken suites counted (silent is not one of them)" \
  || { note FAIL "wrong failure count: $(grep -oE 'FAIL: [0-9]+ of [0-9]+' "$out" | head -1)"; fail=1; }
grep -q "ok1.js\|ok2.js" <(grep '^FAIL' "$out") && { note FAIL "a sound suite was reported as failing"; fail=1; } \
  || note ok "the two sound suites were not reported"

# 3 s: bounded-run's TERM-to-KILL grace on a suite's leftover group
settle(){ local i=0; while [ "$(helpers)" != 0 ] && [ $i -lt 40 ]; do sleep 0.1; i=$((i + 1)); done; }
DEV_SUITE_TIMES="$work/times.tsv" DYNAJS="$work/dyna" DEV_JOBS=2 DEV_SUITE_TIMEOUT=3 DEV_SUITE_CPU_TIMEOUT=3 \
  "$RUNNER" "$work/orphan_exit.js" "$work/orphan_hang.js" >"$work/orphan.log" 2>&1
settle
left=$(helpers)
grep -q "TIMEOUT: .*orphan_hang.js" "$work/orphan.log" && [ "$left" = 0 ] \
  && note ok "a backgrounded helper dies with its suite (normal exit and wall-bound kill)" \
  || { note FAIL "$left helper process(es) outlived their suites (normal exit / wall bound)"; fail=1; pkill -f "$work/helper.sh"; }
DEV_SUITE_TIMES="$work/times.tsv" DYNAJS="$work/dyna" DEV_JOBS=1 DEV_SUITE_TIMEOUT=60 \
  "$RUNNER" "$work/orphan_hang.js" >"$work/orphan2.log" 2>&1 &
rp=$!
i=0; while [ "$(helpers)" = 0 ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
kill -TERM "$rp"; wait "$rp" 2>/dev/null
settle
left=$(helpers)
[ "$left" = 0 ] && note ok "an interrupted runner leaves no suite helper behind" \
  || { note FAIL "$left helper process(es) outlived an interrupted runner"; fail=1; pkill -f "$work/helper.sh"; }

for w in wide1 wide2 wide3; do printf '// flags:\n// slots: 2\n' > "$work/$w.js"; done
printf '// slots: 9\n' > "$work/wide9.js"
wides(){ PEAKS=$1 DYN_TEST_POOL="$work/pool.$1" DYN_TEST_CAP=$2 DYN_TEST_SLOT_HELD= DEV_SUITE_TIMES="$work/times.tsv" DYNAJS="$work/dyna" DEV_JOBS=4 DEV_SUITE_TIMEOUT=20 \
  "$RUNNER" "$work/wide1.js" "$work/wide2.js" "$work/wide3.js" >"$work/wide.$1.log" 2>&1; }
export PEAKS
wides cap2 2; wrc=$?
peak=$(sort -n "$work/peaks.cap2" 2>/dev/null | tail -1); runs=$(wc -l < "$work/peaks.cap2" 2>/dev/null | tr -d ' ')
[ "$wrc" = 0 ] && [ "$runs" = 3 ] && [ "$peak" = 1 ] && note ok "three 'slots: 2' suites under a cap of 2 ran one at a time (peak $peak)" \
  || { note FAIL "'slots: 2' not honoured under a cap of 2: rc=$wrc, $runs ran, peak $peak at once"; fail=1; }
wides cap6 6; wrc=$?
peak=$(sort -n "$work/peaks.cap6" 2>/dev/null | tail -1)
[ "$wrc" = 0 ] && [ "${peak:-0}" -ge 2 ] && note ok "under a cap of 6 they overlap (peak $peak): slots are a weight, not a lock" \
  || { note FAIL "'slots: 2' suites never overlapped under a cap of 6 (rc=$wrc, peak ${peak:-0})"; fail=1; }
PEAKS=cap9 DYN_TEST_POOL="$work/pool.cap9" DYN_TEST_CAP=2 DYN_TEST_SLOT_HELD= DEV_SUITE_TIMES="$work/times.tsv" DYNAJS="$work/dyna" DEV_JOBS=2 DEV_SUITE_TIMEOUT=20 \
  "$RUNNER" "$work/wide9.js" >"$work/wide9.log" 2>&1 && note ok "'slots: 9' under a cap of 2 is clamped and runs" \
  || { note FAIL "'slots: 9' under a cap of 2 did not run"; fail=1; }

echo
if [ "$fail" = 0 ]; then
  echo "runner-selftest: all verdict rules proven (4 failure shapes caught + 1 rejected-rule assertion + no orphaned helper)"
  exit 0
fi
echo "runner-selftest: FAILED -- $fail verdict rule(s) not proven"
exit 1