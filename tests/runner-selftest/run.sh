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
# Usage: tests/runner-selftest/run.sh [path-to-runner]
set -uo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
RUNNER="${1:-$root/tools/run-tests-parallel.sh}"
BOUNDED="$root/tools/bounded-run.sh"
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
esac
exit 0
EOF
chmod +x "$work/dyna"

mk(){ printf '// nothing\n' > "$work/$1.js"; }
mk exits1; mk prints_fail; mk silent; mk spin; mk blocks; mk ok1; mk ok2

echo "== runner verdict selftest (7 stub suites, 5 of them broken)"
out="$work/out.log"
# DEV_SUITE_TIMEOUT=8 so the two unbounded shapes finish quickly; the point is
# that they are bounded AT ALL, not how long the bound is.
DYNAJS="$work/dyna" DEV_JOBS=4 DEV_SUITE_TIMEOUT=8 DEV_SUITE_CPU_TIMEOUT=4 \
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

echo
if [ "$fail" = 0 ]; then
  echo "runner-selftest: all verdict rules proven (4 failure shapes caught + 1 rejected-rule assertion)"
  exit 0
fi
echo "runner-selftest: FAILED -- $fail verdict rule(s) not proven"
exit 1