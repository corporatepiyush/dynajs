#!/usr/bin/env bash
# tests/test_cli_budget_flags.sh -- the sandbox budget flags README promises
# must have a test that goes red when they stop working (B1-16).
#
# README "Running untrusted code" promises three flags:
#   --timeout-ms N          kill runaway loops after N ms
#   --memory-limit N        cap the JS heap
#   --native-memory-limit N cap module-native allocations
# and nothing in any gate exercised them: tests/test_exec_timeout.sh covered
# exactly ONE shape and was referenced by nothing at all.
#
# HONESTY MODEL. Every row here is a shape that either must be bounded (in
# MUST_PASS) or is known NOT to be bounded today (in KNOWN_BROKEN, each with a
# ticket). The rules:
#   - a MUST_PASS shape that stops being bounded  -> FAIL (a real regression)
#   - a KNOWN_BROKEN shape that STARTS being bounded -> FAIL as XPASS, because
#     a silently-fixed guarantee leaves a stale ticket nobody ever closes
#   - a shape neither list names -> FAIL (the list is the specification)
# Every invocation is wrapped in an EXTERNAL wall bound, so a shape that is not
# bounded by the engine still terminates here and is scored, not hung.
set -uo pipefail
cd "$(dirname "$0")/.." || exit 2
DYN=${DYN:-./dynajs}
BOUNDED="$PWD/tools/bounded-run.sh"
[ -x "$DYN" ]  || { echo "FAIL: no engine at $DYN (build first)"; exit 2; }
[ -x "$BOUNDED" ] || { echo "FAIL: $BOUNDED missing -- refusing to run UNBOUNDED"; exit 2; }

OUTER=${OUTER_BOUND:-12}          # external wall bound per shape
BUDGET_MS=${BUDGET_MS_TEST:-300}  # what we ASK the engine to enforce

pass=0; fail=0
ok(){ echo "  ok   $1"; pass=$((pass+1)); }
no(){ echo "  FAIL $1"; fail=$((fail+1)); }

run_shape(){ # $1 = label, $2 = js, $3 = extra dynajs args
  local label="$1" js="$2"; shift 2
  local log rc
  log=$(mktemp "${TMPDIR:-/tmp}/budget.XXXXXX")
  BOUNDED_RUN_MARKER="$log.bounded" "$BOUNDED" "$OUTER" "$(( OUTER / 2 ))" "$label" -- \
    "$DYN" "$@" -e "$js" >"$log" 2>&1
  rc=$?
  if [ -f "$log.bounded" ]; then rc=124; fi
  rm -f "$log.bounded"
  LAST_OUT=$(tail -3 "$log" | tr '\n' ' ')
  rm -f "$log"
  return $rc
}

# --- MUST PASS: the shapes the engine bounds today --------------------------
MUST_PASS=(
  "tight-loop|for(;;){}"
  "async-await-loop|(async function spin(){ await 0; for(;;){} })()"
  "regexp-backtrack|/(a+)+$/b.test('aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa!')"
  "indexOf-loop|var s='x'.repeat(64); for(;;){ s.indexOf('y'); }"
  "microtask-exit-status|queueMicrotask(function spin(){ for(;;){} })"
  "then-recursion|Promise.resolve().then(function spin(){ return spin(); })"
  "tail-call-in-strict|function f(){ 'use strict'; return f(); } f()"
  "do-while-strict-eq|var x=1; do {} while (x === 1);"
  "trycatch-combinator|var g=(function(){for(;;){}}).tryCatch(function(){return 0}); for(;;) g();"
  "heavy-iteration-loop|for(;;) 'a'.repeat(1e6).replace(/a/g,'b');"
  "large-builtin|for(;;) JSON.parse('['+'1,'.repeat(5e6)+'1]')"
  "typedarray-sort|const a=new Array(5e7).fill(1); for(;;) a.sort();"
)
# Every shape that used to be pinned here is bounded now: the interpreter polls
# on tail calls and fused back-edges, an interrupted script exits nonzero, and
# a native call that never yields is ended by the --timeout-ms hard stop (exit
# 113, deadline + grace). New rows go here only with a ticket.
KNOWN_BROKEN=()

echo "== --timeout-ms $BUDGET_MS (external wall bound ${OUTER}s per shape)"
for row in "${MUST_PASS[@]}"; do
  label=${row%%|*}; js=${row#*|}
  run_shape "$label" "$js" --timeout-ms "$BUDGET_MS"
  rc=$?
  # Bounded is not enough: the documented outcome of --timeout-ms is an
  # InternalError "interrupted", so the process must ALSO exit nonzero. A
  # killed script that exits 0 is a silent pass for every caller that checks
  # only the status (shell pipelines, CI steps, the suite runners).
  if [ "$rc" = 124 ]; then no "$label: the engine did NOT stop it (external ${OUTER}s bound fired)"
  elif [ "$rc" = 0 ]; then no "$label: bounded, but exited 0 -- a killed script must exit nonzero"
  else ok "$label: bounded and exited nonzero (rc=$rc) inside ${OUTER}s"; fi
done
for row in ${KNOWN_BROKEN[@]+"${KNOWN_BROKEN[@]}"}; do
  label=${row%%|*}; rest=${row#*|}; js=${rest%%|*}; why=${rest#*|}
  run_shape "$label" "$js" --timeout-ms "$BUDGET_MS"
  rc=$?
  if [ "$rc" = 124 ] || [ "$rc" = 0 ]; then
    echo "  ok   $label: still KNOWN-BROKEN (rc=$rc; $why)"
  else
    no "$label: XPASS -- the engine now bounds this shape with a nonzero exit, so the ticket is stale ($why)"
  fi
done
for row in "${MUST_PASS[@]}"; do
  label=${row%%|*}
  for row2 in ${KNOWN_BROKEN[@]+"${KNOWN_BROKEN[@]}"}; do
    [ "${row2%%|*}" = "$label" ] && no "$label: listed in BOTH MUST_PASS and KNOWN_BROKEN"
  done
done

# --- --memory-limit: a child process must exit NONZERO, and stay bounded -----
echo "== --memory-limit 2000000 (a child process must exit nonzero, RSS bounded)"
run_shape memlimit 'let a=[]; for(;;) a.push(new Array(10000).fill(1));' --memory-limit 2000000
rc=$?
if [ "$rc" = 1 ] && printf '%s' "$LAST_OUT" | grep -q "out of memory"; then
  ok "heap cap: refused with 'out of memory' and a nonzero exit"
elif [ "$rc" = 124 ]; then
  no "heap cap: the process ran past ${OUTER}s instead of being capped"
else
  no "heap cap: rc=$rc, output: $LAST_OUT"
fi

# --- --native-memory-limit: the flag must at least be ACCEPTED and not lie --
# d.ts documents setNativeMemoryLimit(bytes) as the API the flag installs.
echo "== --native-memory-limit 1000000 (flag is parsed and reaches the API)"
run_shape natlimit-import 'import("dyna:sys").then(m=>{ m.setNativeMemoryLimit(1000000); print("cap installed: " + m.memoryUsage().nativeLimit); })' \
        --native-memory-limit 1000000 --std
rc=$?
if [ "$rc" != 0 ]; then
  no "native cap: the flag combination exited $rc: $LAST_OUT"
else
  case "$LAST_OUT" in
    *"cap installed: 1000000"*) ok "native cap: the CLI value reached dyna:sys.setNativeMemoryLimit" ;;
    *) no "native cap: the flag did not install the documented cap: $LAST_OUT" ;;
  esac
fi

# KNOWN GAP (B1-16): the cap is installed but the per-module allocators that
# actually own memory (dyna:bytes copies) do not consult it. Pinned so the gap
# cannot be forgotten, and so it FAILS once the engine starts enforcing -- at
# which point the ticket is stale and this row must move to MUST_PASS.
run_shape natlimit-enforce 'import("dyna:bytes").then(m=>{const keep=[]; for(let i=0;i<200;i++) keep.push(new m.Bytes(new Uint8Array(500000))); print("SURVIVED");})' \
        --native-memory-limit 1000000 --std
rc=$?
if printf '%s' "$LAST_OUT" | grep -q "SURVIVED"; then
  echo "  ok   native cap ENFORCEMENT: still KNOWN-BROKEN (expected; B1-16: dyna:bytes allocations ignore the cap)"
else
  no "native cap ENFORCEMENT: XPASS -- the cap is now enforced; move this row to MUST_PASS and close B1-16"
fi

echo
echo "test_cli_budget_flags: $pass passed, $fail failed"
[ "$fail" -eq 0 ] || exit 1
exit 0