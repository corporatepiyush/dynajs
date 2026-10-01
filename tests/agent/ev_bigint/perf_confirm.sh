#!/bin/zsh
OUT=ev/perf_confirm.txt
: > $OUT
run_case() {
  local mode=$1 digits=$2 iters=$3 radix=$4
  local best_p=999999999 best_c=999999999
  for rep in 1 2 3; do
    local tp=$( /Users/piyush/ai/dynascript/.agent-work/work12/dynajs ev/bench.js $mode $digits $iters $radix | grep -E '^(MUL|MULBAL|TOSTR|ADD)' | awk '{print $NF}' )
    local tc=$( /Users/piyush/ai/dynascript/.agent-work/work12_base/dynajs ev/bench.js $mode $digits $iters $radix | grep -E '^(MUL|MULBAL|TOSTR|ADD)' | awk '{print $NF}' )
    (( tp < best_p )) && best_p=$tp
    (( tc < best_c )) && best_c=$tc
  done
  local ratio=$(python3 -c "print(round($best_c/$best_p,2))")
  echo "$mode d=$digits r=$radix iters=$iters patched=${best_p}ms control=${best_c}ms speedup=$ratio" | tee -a $OUT
}
echo "=== confirm high-iters ===" | tee -a $OUT
run_case mul 10000 300
run_case mul 40000 40
run_case mulbal 10000 300
run_case mulbal 40000 40
run_case tostr 10000 400 10
run_case tostr 40000 60 10
run_case mulbal 500 20000
