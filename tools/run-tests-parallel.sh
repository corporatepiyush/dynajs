#!/usr/bin/env bash
# tools/run-tests-parallel.sh -- parallel test runner for dynascript test suites
set -uo pipefail

JOBS=${DEV_JOBS:-$( (command -v nproc >/dev/null 2>&1 && nproc) || sysctl -n hw.ncpu 2>/dev/null || echo 4 )}
DYNAJS=${DYNAJS:-./dynajs}
EXTRA_ARGS=${EXTRA_ARGS:-""}

if [ $# -eq 0 ]; then
  echo "Usage: $0 [test_file.js ...]" >&2
  exit 1
fi

[ -x "$DYNAJS" ] || {
  echo "FAIL: binary $DYNAJS not found or not executable" >&2
  exit 1
}

# Identify tests that must run solo (fixed port bindings, shared /tmp resources)
SOLO_RE='(PostgreSQL|Redis|SQLite|TCPServer|HTTPServer|HTTPServerAsync|DNSServer|test_http_params|test_http_pentest|test_net_pentest|test_http_proxy|test_http_hardening)'

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
    TMPDIR="$SCRATCH_DIR" $DYNAJS $EXTRA_ARGS "$t" </dev/null >"$out_file" 2>&1
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
  TMPDIR="$SCRATCH_DIR" $DYNAJS $EXTRA_ARGS "$t" </dev/null >"$out_file" 2>&1
  echo "$? $t" >"$rc_file"
done

# Collect results and display failures
failed_tests=()
for rc_file in "$TMPDIR_ROOT"/*.rc; do
  [ -e "$rc_file" ] || continue
  read -r rc cmd < "$rc_file"
  if [ "$rc" -ne 0 ]; then
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
