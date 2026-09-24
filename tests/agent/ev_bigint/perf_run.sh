#!/bin/zsh
# perf_run.sh — interleaved 3-rep min benchmark: patched vs control (work12_base)
# usage: perf_run.sh <outfile>
OUT=${1:-/tmp/perf_results.txt}
PATCHED=/Users/piyush/ai/dynascript/.agent-work/work12/dynajs
CONTROL=/Users/piyush/ai/dynascript/.agent-work/work12_base/dynajs
BENCH=/Users/piyush/ai/dynascript/.agent-work/work12/ev/bench.js
: > $OUT

run_case() { # mode digits iters radix
  local mode=$1 digits=$2 iters=$3 radix=$4
  local best_p=999999999 best_c=999999999
  for rep in 1 2 3; do
    # interleave: patched, control, patched, control, ...
    local tp=$( $PATCHED $BENCH $mode $digits $iters $radix | grep -E '^(MUL|MULBAL|TOSTR)' | awk '{print $NF}' )
    local tc=$( $CONTROL $BENCH $mode $digits $iters $radix | grep -E '^(MUL|MULBAL|TOSTR)' | awk '{print $NF}' )
    (( tp < best_p )) && best_p=$tp
    (( tc < best_c )) && best_c=$tc
  done
  local ratio="n/a"
  if (( best_c > 0 )); then ratio=$(echo "scale=2; $best_c / $best_p" | bc 2>/dev/null || python3 -c "print(round($best_c/$best_p,2))"); fi
  local line="$mode d=$digits r=$radix iters=$iters patched=${best_p}ms control=${best_c}ms speedup=$ratio"
  echo "$line" | tee -a $OUT
}

echo "=== mul ladder (unbalanced: d x d/2) ===" | tee -a $OUT
run_case mul 1000 3000
run_case mul 2000 1000
run_case mul 4000 300
run_case mul 10000 60
run_case mul 40000 8

echo "=== mul ladder (balanced: d x d) ===" | tee -a $OUT
run_case mulbal 1000 3000
run_case mulbal 2000 1000
run_case mulbal 4000 300
run_case mulbal 10000 60
run_case mulbal 40000 8

echo "=== small mul regression watch (balanced) ===" | tee -a $OUT
run_case mulbal 50 50000
run_case mulbal 100 30000
run_case mulbal 200 15000
run_case mulbal 500 4000

echo "=== crossover (balanced mul) ===" | tee -a $OUT
run_case mulbal 200 20000
run_case mulbal 300 15000
run_case mulbal 400 10000
run_case mulbal 600 5000
run_case mulbal 800 3000
run_case mulbal 1200 2000

echo "=== toString(10) ladder ===" | tee -a $OUT
run_case tostr 500 10000 10
run_case tostr 1000 4000 10
run_case tostr 2000 1500 10
run_case tostr 4000 400 10
run_case tostr 10000 80 10
run_case tostr 20000 20 10
run_case tostr 40000 8 10

echo "=== toString crossover ===" | tee -a $OUT
run_case tostr 600 6000 10
run_case tostr 800 5000 10
run_case tostr 1000 4000 10
run_case tostr 1200 3000 10
run_case tostr 1500 2500 10
run_case tostr 3000 800 10

echo "=== toString(16) unchanged (binary fast path) ===" | tee -a $OUT
run_case tostr 10000 100 16
run_case tostr 40000 30 16
echo "DONE" | tee -a $OUT
