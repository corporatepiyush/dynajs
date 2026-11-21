#!/usr/bin/env bash
# tools/run-tests-parallel.sh -- parallel test runner for dynascript test suites.
# Fan a list of test files out over $DYNAJS with a bounded worker pool; the
# exit is 0 only when every suite passed. Used by tools/xplat-full.js
# and by hand for any list of suites (tests/suites.sh lists them all).
#
# Usage: tools/run-tests-parallel.sh test_file.js [test_file.js ...]
#   DYNAJS=<bin> (default ./dynajs)   EXTRA_ARGS="--std ..." forwarded as flags
#   DEV_JOBS=N     worker pool size (default: CPU count)
#
# Per-suite declarations, read from the FIRST 3 LINES of each test file:
#   // flags: --std        extra interpreter flags for that suite alone
#   // timeout: 120        wall-seconds bound (CPU bound = half of it); a
#                         declaration past line 3 or a malformed value is a
#                         loud WARNING and the suite falls back to the default
#                         bound below
# Every suite is bounded. A suite that declares no bound used to run with NO
# external bound at all, so 447 of 508 gated suites could hang a stage until
# the stage wall -- and on a machine with no timeout(1) there is no stage wall
# either (B1-06.4). DEV_SUITE_TIMEOUT sets that default (seconds, 120).
#
# Bounds are enforced externally by tools/bounded-run.sh, so exit 124/125
# mean TIMEOUT (wall/CPU) -- reported distinctly from a suite's own 124/125,
# via the BOUNDED_RUN_MARKER file.
#
# Concurrency safety: suites matching SOLO_RE (fixed port bindings: PostgreSQL,
# Redis, SQLite, TCPServer/HTTPServer/DNS, the pentest/proxy/hardening/TLS
# suites...) or sharing a duplicated /tmp path with another listed suite run
# SOLO, after the parallel wave; everything else runs in parallel with its own
# scratch TMPDIR and worker dir.
#
# Output: one summary line on success ("all N test suites passed (s solo, p
# parallel)"); on failure one FAIL/TIMEOUT box per suite (last 30 lines of its
# output) plus a final list, exit 1.
#
# A suite that exits 0 but printed a per-check failure marker (a line whose
# first word is FAIL, e.g. "FAIL: ..." / "  FAIL  ...") is ALSO a failure: some
# suites count failures but never propagate them, and a silent pass here would
# mask a red assertion. A suite that exits 0 having printed NOTHING is accepted
# (see the note at the verdict loop): the engine exits non-zero on an uncaught
# error, an unhandled rejection and an await that can never settle, which are
# the ways a chain stops early.
set -uo pipefail

JOBS=${DEV_JOBS:-$( (command -v nproc >/dev/null 2>&1 && nproc) || sysctl -n hw.ncpu 2>/dev/null || echo 4 )}
DYNAJS=${DYNAJS:-./dynajs}
EXTRA_ARGS=${EXTRA_ARGS:-""}
BOUNDED_RUN="$(dirname "$0")/bounded-run.sh"
TEST_SLOT="$(dirname "$0")/test-slot.sh"
SUITE_TIMEOUT=${DEV_SUITE_TIMEOUT:-120}
# DEV_SUITE_TIME_SCALE multiplies EVERY suite bound (declared or default). It is
# for a host that is uniformly slower than the one the bounds were written on --
# CPU emulation above all -- where a native budget measures the emulator.
SUITE_TIME_SCALE=${DEV_SUITE_TIME_SCALE:-1}
case "$SUITE_TIME_SCALE" in ''|*[!0-9]*|0) echo "FAIL: DEV_SUITE_TIME_SCALE must be a positive integer (got '$SUITE_TIME_SCALE')" >&2; exit 2 ;; esac
# CPU-time bound per suite: DEV_SUITE_CPU_TIMEOUT when set (scaled like the
# wall bound, never above it), otherwise half the suite's wall bound.
SUITE_CPU_TIMEOUT=${DEV_SUITE_CPU_TIMEOUT:-}
case "$SUITE_CPU_TIMEOUT" in ''|[1-9]*) ;; *) echo "FAIL: DEV_SUITE_CPU_TIMEOUT must be a positive integer (got '$SUITE_CPU_TIMEOUT')" >&2; exit 2 ;; esac
case "$SUITE_CPU_TIMEOUT" in *[!0-9]*) echo "FAIL: DEV_SUITE_CPU_TIMEOUT must be a positive integer (got '$SUITE_CPU_TIMEOUT')" >&2; exit 2 ;; esac

if [ $# -eq 0 ]; then
  echo "Usage: $0 [test_file.js ...]" >&2
  exit 1
fi

[ -x "$DYNAJS" ] || {
  echo "FAIL: binary $DYNAJS not found or not executable" >&2
  exit 1
}

SOLO_RE='(PostgreSQL|Redis|SQLite|TCPServer|HTTPServer|HTTPServerAsync|DNSServer|test_http_params|test_http_pentest|test_net_pentest|test_http_proxy|test_http_hardening|test_client_tls|test_onconnect_hook|^//[[:space:]]*solo:)'

# One pass over a suite's first lines with shell builtins (no head/grep/sed
# per suite: the runner's own forks were ~80 ms CPU per suite, nine times the
# engine's start-up). Sets HDR_FLAGS, HDR_TMO and HDR_SLOTS for run_suite.
# "// slots: N" (first 10 lines) is how many machine-wide test slots the suite
# takes (tools/test-slot.sh -n N): a suite that keeps a child dynajs, a mock
# server or another helper process running beside itself declares 2 or more,
# so DYN_TEST_CAP bounds processes, not suites (F-cap-1).
suite_header() {
  local f="$1" line i=0 val tline=0 tval="" misplaced=0
  HDR_FLAGS=""
  HDR_TMO=$SUITE_TIMEOUT
  HDR_SLOTS=1
  local got_flags=0
  while [ "$i" -lt 10 ] && IFS= read -r line; do
    i=$((i + 1))
    if [ "$i" -le 3 ]; then
      if [ "$got_flags" = 0 ] && [[ $line =~ ^//\ *flags:(.*)$ ]]; then
        HDR_FLAGS=${BASH_REMATCH[1]}; got_flags=1
      fi
      if [ "$tline" = 0 ] && [[ $line =~ ^//[[:space:]]*timeout:[[:space:]]*(.*)$ ]]; then
        tline=$i; tval=${BASH_REMATCH[1]}
      fi
    elif [ "$tline" = 0 ] && [ "$misplaced" = 0 ] && [[ $line =~ ^//[[:space:]]*timeout: ]]; then
      misplaced=$i
    fi
    if [[ $line =~ ^//[[:space:]]*slots:[[:space:]]*(.*)$ ]]; then
      case ${BASH_REMATCH[1]} in
        ''|*[!0-9]*|0) echo "WARNING: $f:$i: malformed '// slots:' declaration ('// slots: ${BASH_REMATCH[1]}'); taking 1 slot" >&2 ;;
        *) HDR_SLOTS=${BASH_REMATCH[1]} ;;
      esac
    fi
  done < "$f"
  if [ "$tline" = 0 ]; then
    [ "$misplaced" != 0 ] && echo "WARNING: $f:$misplaced: '// timeout:' is read only from the first 3 lines; this suite runs under the DEFAULT ${SUITE_TIMEOUT}s bound; move the declaration to the top" >&2
    return 0
  fi
  case $tval in
    ''|*[!0-9]*)
      echo "WARNING: $f:$tline: malformed '// timeout:' declaration ('// timeout: $tval'); falling back to the ${SUITE_TIMEOUT}s default bound" >&2 ;;
    *) HDR_TMO=$tval ;;
  esac
}

run_suite() {
  local t="$1" out_file="$2" scratch="$3" bound_marker="$4" flags tmo cpu
  suite_header "$t"
  flags=$HDR_FLAGS
  tmo=$(( HDR_TMO * SUITE_TIME_SCALE ))
  if [ -n "$SUITE_CPU_TIMEOUT" ]; then
    cpu=$(( SUITE_CPU_TIMEOUT * SUITE_TIME_SCALE ))
    [ "$cpu" -gt "$tmo" ] && cpu=$tmo
  else
    cpu=$((tmo / 2))
  fi
  [ "$cpu" -lt 1 ] && cpu=1
  # EVERY suite is externally bounded. The two spellings are the same
  # instrument (tools/bounded-run.sh); a wall kill is labelled 124 and a CPU
  # kill 125, and the marker file is what distinguishes "the bound fired"
  # from "the suite exited 124 on its own".
  if [ ! -x "$BOUNDED_RUN" ]; then
    echo "FAIL: $BOUNDED_RUN is missing or not executable; refusing to run $t UNBOUNDED" >&2
    return 2
  fi
  # shellcheck disable=SC2086  # flags is a word list by design
  # The machine-wide cap (tools/test-slot.sh, DYN_TEST_CAP): the slot is taken
  # BEFORE the bound starts, so waiting for one never counts as suite time.
  TMPDIR="$scratch" BOUNDED_RUN_MARKER="$bound_marker" "$TEST_SLOT" -n "$HDR_SLOTS" -- "$BOUNDED_RUN" "$tmo" "$cpu" "$t" -- \
    $DYNAJS $EXTRA_ARGS $flags "$t" </dev/null >"$out_file" 2>&1
}

# Longest first: a suite's last measured wall time (DEV_SUITE_TIMES, written
# at the end of every run) orders both waves, so the long suites start while
# the pool is wide instead of finishing alone at the end. A suite with no
# record sorts first -- it is as likely as any to be long.
SUITE_TIMES=${DEV_SUITE_TIMES:-.prepush/suite-times.tsv}
now_s(){ if [ -n "${EPOCHREALTIME:-}" ]; then printf '%s' "${EPOCHREALTIME/,/.}"; else printf '%s' "$SECONDS"; fi; }
now_us(){ if [ -n "${EPOCHREALTIME:-}" ]; then NOW_US=${EPOCHREALTIME//[.,]/}; else NOW_US=$((SECONDS * 1000000)); fi; }
# run_timed N T: one suite in worker N, its verdict and duration in files, all
# with builtins (the duration used to cost two subshells and an awk).
run_timed() {
  local k="$1" t="$2" a
  mkdir -p "$TMPDIR_ROOT/worker_$k"
  now_us; a=$NOW_US
  run_suite "$t" "$TMPDIR_ROOT/$k.out" "$TMPDIR_ROOT/worker_$k" "$TMPDIR_ROOT/$k.bounded"
  local rc=$?
  echo "$rc $t" >"$TMPDIR_ROOT/$k.rc"
  now_us
  a=$(( (NOW_US - a + 5000) / 10000 ))
  printf '%d.%02d\t%s\n' $((a / 100)) $((a % 100)) "$t" >"$TMPDIR_ROOT/$k.time"
}
longest_first(){
  [ "$#" -gt 0 ] || return 0
  printf '%s\n' "$@" | awk -F'\t' 'FILENAME != "-" { v[$2] = $1; next } { print ($0 in v ? v[$0] : 1e9) "\t" $0 }' "$SUITE_TIMES" - \
    | sort -t$'\t' -k1,1gr | cut -f2-
}

solo_list=()
parallel_list=()

tmp_dups=$(grep -ohE '/tmp/[A-Za-z0-9_.-]+' "$@" 2>/dev/null | sort | uniq -d || true)

for t in "$@"; do
  [ -f "$t" ] || { echo "FAIL: test file $t not found" >&2; exit 1; }
done
declare -A solo_hit=()
while IFS= read -r t; do solo_hit[$t]=1; done < <(grep -lE "$SOLO_RE" "$@" 2>/dev/null)
if [ -n "$tmp_dups" ]; then
  while IFS= read -r t; do solo_hit[$t]=1; done < <(grep -lF -- "$tmp_dups" "$@" 2>/dev/null)
fi
for t in "$@"; do
  is_solo=0
  if [[ "$t" =~ $SOLO_RE ]] || [ -n "${solo_hit[$t]:-}" ]; then
    is_solo=1
  fi
  if [ "$is_solo" -eq 1 ]; then
    solo_list+=("$t")
  else
    parallel_list+=("$t")
  fi
done

if [ -f "$SUITE_TIMES" ]; then
  mapfile -t parallel_list < <(longest_first ${parallel_list[@]+"${parallel_list[@]}"})
  mapfile -t solo_list < <(longest_first ${solo_list[@]+"${solo_list[@]}"})
fi
# the ordering must not lose a suite: an empty times file once made the
# reorder print nothing, and the run passed "all 0 test suites"
if [ "$(( ${#parallel_list[@]} + ${#solo_list[@]} ))" != "$#" ]; then
  echo "FAIL: runner integrity -- $# suites given, $(( ${#parallel_list[@]} + ${#solo_list[@]} )) scheduled" >&2
  exit 1
fi

TMPDIR_ROOT=$(mktemp -d "/tmp/dyna_test_XXXXXX")
# Interrupted: TERM every worker's bounded-run, whose own trap TERMs (then
# KILLs) its suite's whole process group, helpers included -- a worker shell
# killed alone left its suite and everything the suite started running.
stop_workers(){ local j; for j in $(jobs -p); do pkill -TERM -P "$j" 2>/dev/null; kill -TERM "$j" 2>/dev/null; done; wait; }
trap 'rm -rf "$TMPDIR_ROOT"' EXIT
trap 'stop_workers; exit 130' INT TERM

n=0
fail=0

ph0=$(now_s)
running=0
for t in ${parallel_list[@]+"${parallel_list[@]}"}; do
  if [ "$running" -ge "$JOBS" ]; then
    wait -n
    running=$((running - 1))
  fi
  n=$((n + 1))
  printf '%s\n' "$t" >"$TMPDIR_ROOT/$n.name"
  run_timed "$n" "$t" &
  running=$((running + 1))
done

wait
ph1=$(now_s)

# The "solo" suites start servers. Nearly all of them bind port 0 and keep
# their files in a private scratch directory, so they do not clash with each
# other -- they were serial only because they are timing-sensitive under the
# full-width wave. They run in a second, NARROW wave of DEV_SOLO_JOBS at a time
# (1 = strictly serial, the historical behaviour; default 8, never more than
# the worker pool). Measured on 495 suites: 702 s at 1, 360 s at 4, 293 s at 8.
# On 646 suites once the slow ones were split and their idle timers cleared
# (2026-10-17): 226 s at 4, 170 s at 8 (twice, all passing). The
# suites that talk to one shared external service on a fixed port stay
# strictly serial whatever the width: two of them at once would be talking to
# each other's data.
SOLO_JOBS=${DEV_SOLO_JOBS:-$(( JOBS < 8 ? JOBS : 8 ))}
case "$SOLO_JOBS" in ''|*[!0-9]*|0) echo "FAIL: DEV_SOLO_JOBS must be a positive integer (got '$SOLO_JOBS')" >&2; exit 2 ;; esac
EXCLUSIVE_RE='(PostgreSQL|Redis|redis-server|56379|55432|pg_ctl)'
exclusive_list=()
others=()
for t in ${solo_list[@]+"${solo_list[@]}"}; do
  if [ "$SOLO_JOBS" -gt 1 ] && ! grep -qE "$EXCLUSIVE_RE" "$t" 2>/dev/null; then
    others+=("$t")
  else
    exclusive_list+=("$t")
  fi
done

# The suites that share one external service exclude EACH OTHER, not the rest:
# they run as one serial chain beside the solo wave, holding one of its slots.
ex_idx=()
for t in ${exclusive_list[@]+"${exclusive_list[@]}"}; do
  n=$((n + 1))
  ex_idx+=("$n")
  printf '%s\n' "$t" >"$TMPDIR_ROOT/$n.name"
done
running=0
if [ "${#exclusive_list[@]}" -gt 0 ]; then
  (
    i=0
    for t in "${exclusive_list[@]}"; do
      k=${ex_idx[$i]}; i=$((i + 1))
      run_timed "$k" "$t"
    done
  ) &
  running=1
fi

for t in ${others[@]+"${others[@]}"}; do
  if [ "$running" -ge "$SOLO_JOBS" ]; then
    wait -n
    running=$((running - 1))
  fi
  n=$((n + 1))
  printf '%s\n' "$t" >"$TMPDIR_ROOT/$n.name"
  run_timed "$n" "$t" &
  running=$((running + 1))
done

wait
ph2=$(now_s)
phases=$(awk -v a="$ph0" -v b="$ph1" -v c="$ph2" 'BEGIN { printf "parallel %.0fs, solo with the serial chain beside it %.0fs", b - a, c - b }')

# merge this run's durations into the record (newest wins, others kept)
if mkdir -p "$(dirname "$SUITE_TIMES")" 2>/dev/null; then
  { cat "$TMPDIR_ROOT"/*.time 2>/dev/null; if [ -f "$SUITE_TIMES" ]; then cat "$SUITE_TIMES"; fi; } \
    | awk -F'\t' 'NF == 2 && $2 !~ /^\// && !seen[$2]++' >"$SUITE_TIMES.tmp" 2>/dev/null \
    && mv "$SUITE_TIMES.tmp" "$SUITE_TIMES"
fi

failed_tests=()
# Every LAUNCHED suite must have left a verdict. A worker killed before it
# could write its rc file used to be simply absent, and the summary still said
# "all N test suites passed" with N counting launches, not verdicts (B1-06.5).
launched=$n
verdicts=$(ls "$TMPDIR_ROOT"/*.rc 2>/dev/null | wc -l | tr -d ' ')
if [ "$verdicts" != "$launched" ]; then
  fail=$((fail + 1))
  failed_tests+=("runner integrity: $verdicts of $launched launched suites wrote a verdict (a worker died before reporting)")
  echo "-----------------------------------------------------------------"
  echo "FAIL: runner integrity -- $verdicts verdict files for $launched launched suites"
  echo "A worker was killed before it could report. This is NOT a pass."
  echo "-----------------------------------------------------------------"
fi
for rc_file in "$TMPDIR_ROOT"/*.rc; do
  [ -e "$rc_file" ] || continue
  read -r rc cmd < "$rc_file"
  bound_marker="${rc_file%.rc}.bounded"
  verdict=$rc
  # a verdict that is not an exit status is a failure, never a pass
  case "$rc" in
    ''|*[!0-9]*)
      fail=$((fail + 1))
      name=$(cat "${rc_file%.rc}.name" 2>/dev/null)
      failed_tests+=("${name:-?} (verdict file ${rc_file##*/} holds '$rc' -- something overwrote it)")
      echo "-----------------------------------------------------------------"
      echo "FAIL: ${name:-?}: its verdict file ${rc_file##*/} holds '$rc' -- something overwrote the runner's own file"
      echo "-----------------------------------------------------------------"
      continue ;;
  esac
  if [ "$rc" -eq 124 ] || [ "$rc" -eq 125 ]; then
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
  else
    out_file="${rc_file%.rc}.out"
    if [ -f "$out_file" ] && grep -qE '^[[:space:]]*FAIL([: ]|$)' "$out_file"; then
      fail=$((fail + 1))
      verdict=1
      failed_tests+=("$cmd (printed FAIL but exited 0 -- suite does not propagate failure)")
      echo "-----------------------------------------------------------------"
      echo "FAIL: $cmd (printed FAIL but exited 0 -- the suite's checks failed without a nonzero exit)"
      echo "Output:"
      tail -30 "$out_file" 2>/dev/null | sed 's/^/  /'
      echo "-----------------------------------------------------------------"
    else
      :   # ran, claimed no failure: accepted.
      #
      # TRIED AND REJECTED: treating "exited 0 with NO output" as a failure.
      # It is a false positive: a suite whose only failure signal is a throw is
      # CORRECT to print nothing when everything passes. Measured on the real
      # core stage: 6 of 45 suites (test_closure, test_language, test_loop,
      # test_bigint, test_cyclic_import, test_worker) are legitimately silent,
      # and the rule turned a green stage red on all six. The runner cannot
      # distinguish "passed silently" from "a callback chain died before its
      # first line" without a REQUIRED verdict marker every suite must print --
      # that is an edit to ~450 suites and is ticket B1-06.6-required-marker.
    fi
  fi
done

if [ "$fail" -eq 0 ]; then
  echo "  all $n test suites passed (${#solo_list[@]} solo [${#exclusive_list[@]} strictly serial, rest $SOLO_JOBS at a time], ${#parallel_list[@]} parallel; $phases)"
  exit 0
else
  echo "FAIL: $fail of $n test suites failed:"
  for ft in "${failed_tests[@]}"; do
    echo "  - $ft"
  done
  exit 1
fi
