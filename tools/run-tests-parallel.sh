#!/usr/bin/env bash
# tools/run-tests-parallel.sh -- parallel test runner for dynascript test suites
set -uo pipefail

JOBS=${DEV_JOBS:-$( (command -v nproc >/dev/null 2>&1 && nproc) || sysctl -n hw.ncpu 2>/dev/null || echo 4 )}
DYNAJS=${DYNAJS:-./dynajs}
EXTRA_ARGS=${EXTRA_ARGS:-""}
BOUNDED_RUN="$(dirname "$0")/bounded-run.sh"

if [ $# -eq 0 ]; then
  echo "Usage: $0 [test_file.js ...]" >&2
  exit 1
fi

[ -x "$DYNAJS" ] || {
  echo "FAIL: binary $DYNAJS not found or not executable" >&2
  exit 1
}

# Identify tests that must run solo (fixed port bindings, shared /tmp resources)
SOLO_RE='(PostgreSQL|Redis|SQLite|TCPServer|HTTPServer|HTTPServerAsync|DNSServer|test_http_params|test_http_pentest|test_net_pentest|test_http_proxy|test_http_hardening|test_client_tls|test_onconnect_hook)'

# Per-suite extra interpreter flags (P6 parity): a test needing --std (or any
# flag) declares it with a first-line comment:  // flags: --std
suite_flags() {
  local flags
  flags=$(head -3 "$1" 2>/dev/null | grep -E '^// *flags:' | head -1 | sed -E 's|^//[[:space:]]*flags:||')
  echo "$flags"
}

# Per-suite EXTERNAL bound (wall clock AND CPU time), declared with a comment
# in the first 3 lines:
#   // timeout: 120
# An in-test watchdog only fires while the event loop turns; a synchronous
# busy-spin or a wedged syscall wedges the suite silently. tools/bounded-run.sh
# terminates the run from outside and reports it as TIMEOUT (exit 124 wall /
# 125 CPU), distinct from the suite's own failure. Empty = unbounded, as
# before. Sizing rule: well above the suite's in-test watchdog.
#
# A declaration that is present but unreadable -- a malformed value (trailing
# space, extra text) or a header outside the scanned lines -- must NOT degrade
# to unbounded silently: that is how a wedged suite hangs the gate. Both warn
# loudly, naming the file and line, and the suite then runs unbounded.
suite_timeout() {
  local hit lineno val misplaced
  hit=$(head -3 "$1" 2>/dev/null | grep -nE '^//[[:space:]]*timeout:' | head -1)
  if [ -z "$hit" ]; then
    misplaced=$(head -10 "$1" 2>/dev/null | grep -nE '^//[[:space:]]*timeout:' | head -1)
    if [ -n "$misplaced" ]; then
      echo "WARNING: $1:${misplaced%%:*}: '// timeout:' is read only from the first 3 lines -- this suite runs UNBOUNDED; move the declaration to the top" >&2
    fi
    echo ""
    return
  fi
  lineno=${hit%%:*}
  val=$(printf '%s\n' "${hit#*:}" | sed -E 's|^//[[:space:]]*timeout:[[:space:]]*||')
  case $val in
    ''|*[!0-9]*)
      echo "WARNING: $1:$lineno: malformed '// timeout:' declaration ('// timeout: $val') -- this suite runs UNBOUNDED; write '// timeout: <seconds>'" >&2
      echo ""
      ;;
    *) echo "$val" ;;
  esac
}

# One suite, one row: run it through bounded-run.sh when it declared a timeout.
# $4 is the caller-visible bound marker: bounded-run.sh creates it only when
# ITS bound fired, so the report can label a 124/125 as the suite's own exit
# when it did not.
run_suite() {
  local t="$1" out_file="$2" scratch="$3" bound_marker="$4" flags tmo cpu
  flags=$(suite_flags "$t")
  tmo=$(suite_timeout "$t")
  if [ -n "$tmo" ]; then
    # The CPU bound is half the wall bound, but never zero: a declared bound
    # of 1s would otherwise be rejected as an invalid bound instead of run.
    cpu=$((tmo / 2))
    [ "$cpu" -lt 1 ] && cpu=1
    # shellcheck disable=SC2086  # flags is a word list by design
    TMPDIR="$scratch" BOUNDED_RUN_MARKER="$bound_marker" "$BOUNDED_RUN" "$tmo" "$cpu" "$t" -- \
      $DYNAJS $EXTRA_ARGS $flags "$t" </dev/null >"$out_file" 2>&1
  else
    # shellcheck disable=SC2086
    TMPDIR="$scratch" $DYNAJS $EXTRA_ARGS $flags "$t" </dev/null >"$out_file" 2>&1
  fi
}

solo_list=()
parallel_list=()

# Find duplicate temp paths across test files
tmp_dups=$(grep -ohE '/tmp/[A-Za-z0-9_.-]+' "$@" 2>/dev/null | sort | uniq -d || true)

for t in "$@"; do
  [ -f "$t" ] || { echo "FAIL: test file $t not found" >&2; exit 1; }
  is_solo=0
  if [[ "$t" =~ $SOLO_RE ]] || grep -qE "$SOLO_RE" "$t" 2>/dev/null; then
    is_solo=1
  elif [ -n "$tmp_dups" ] && grep -qF -- "$tmp_dups" "$t" 2>/dev/null; then
    is_solo=1
  fi
  if [ "$is_solo" -eq 1 ]; then
    solo_list+=("$t")
  else
    parallel_list+=("$t")
  fi
done

TMPDIR_ROOT=$(mktemp -d "/tmp/dyna_test_XXXXXX")
trap 'rm -rf "$TMPDIR_ROOT"' EXIT INT TERM

n=0
fail=0

# Run parallel batch
for t in "${parallel_list[@]}"; do
  while [ "$(jobs -rp | wc -l)" -ge "$JOBS" ]; do
    sleep 0.02
  done
  n=$((n + 1))
  out_file="$TMPDIR_ROOT/$n.out"
  rc_file="$TMPDIR_ROOT/$n.rc"
  (
    # Each worker gets an isolated scratch environment
    SCRATCH_DIR="$TMPDIR_ROOT/worker_$n"
    mkdir -p "$SCRATCH_DIR"
    run_suite "$t" "$out_file" "$SCRATCH_DIR" "$TMPDIR_ROOT/$n.bounded"
    echo "$? $t" >"$rc_file"
  ) &
done

wait

# Run solo tests sequentially
for t in "${solo_list[@]}"; do
  n=$((n + 1))
  out_file="$TMPDIR_ROOT/$n.out"
  rc_file="$TMPDIR_ROOT/$n.rc"
  SCRATCH_DIR="$TMPDIR_ROOT/worker_$n"
  mkdir -p "$SCRATCH_DIR"
  run_suite "$t" "$out_file" "$SCRATCH_DIR" "$TMPDIR_ROOT/$n.bounded"
  echo "$? $t" >"$rc_file"
done

# Collect results and display failures
failed_tests=()
for rc_file in "$TMPDIR_ROOT"/*.rc; do
  [ -e "$rc_file" ] || continue
  read -r rc cmd < "$rc_file"
  bound_marker="${rc_file%.rc}.bounded"
  if [ "$rc" -eq 124 ] || [ "$rc" -eq 125 ]; then
    # A TIMEOUT is reported distinctly from a failure: the run was killed by
    # the external wall-clock (124) or CPU-time (125) bound -- a hang, not the
    # suite's own verdict. The marker says whether the bound actually fired;
    # without it, 124/125 is the SUITE's own exit and is labelled as a failure
    # that merely shares the number. Either way it fails the gate.
    fail=$((fail + 1))
    out_file="${rc_file%.rc}.out"
    if [ -f "$bound_marker" ]; then
      failed_tests+=("TIMEOUT: $cmd (external bound, exit $rc)")
      echo "-----------------------------------------------------------------"
      echo "TIMEOUT: $cmd (external bound, exit $rc)"
      echo "Output:"
      tail -30 "$out_file" 2>/dev/null | sed 's/^/  /'
      echo "-----------------------------------------------------------------"
    else
      failed_tests+=("$cmd (exit $rc -- the suite's OWN exit; no external bound fired)")
      echo "-----------------------------------------------------------------"
      echo "FAIL: $cmd (exit $rc -- the suite's OWN exit; no external bound fired)"
      echo "Output:"
      tail -30 "$out_file" 2>/dev/null | sed 's/^/  /'
      echo "-----------------------------------------------------------------"
    fi
  elif [ "$rc" -ne 0 ]; then
    fail=$((fail + 1))
    failed_tests+=("$cmd (exit $rc)")
    out_file="${rc_file%.rc}.out"
    echo "-----------------------------------------------------------------"
    echo "FAIL: $cmd (exit $rc)"
    echo "Output:"
    tail -30 "$out_file" 2>/dev/null | sed 's/^/  /'
    echo "-----------------------------------------------------------------"
  fi
done

if [ "$fail" -eq 0 ]; then
  echo "  all $n test suites passed (${#solo_list[@]} solo, ${#parallel_list[@]} parallel)"
  exit 0
else
  echo "FAIL: $fail of $n test suites failed:"
  for ft in "${failed_tests[@]}"; do
    echo "  - $ft"
  done
  exit 1
fi
