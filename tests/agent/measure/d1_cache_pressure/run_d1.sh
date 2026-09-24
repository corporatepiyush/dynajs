#!/bin/zsh
# D1 cache-pressure study runner.
# Runs the engine working-set kernels (min-of-5 process invocations each,
# every invocation under timeout) plus the C calibration bench, producing
# d1_results.tsv next to this script.
#
# Usage: run_d1.sh [dynajs-binary]   (default: the tree's ./dynajs)
set -u
cd "$(dirname "$0")/../../../.."   # tree root (script lives 4 levels deep)
DYNA=${1:-./dynajs}
HERE=tests/agent/measure/d1_cache_pressure
OUT=$HERE/d1_results.tsv
K=5            # process-level min-of-K
TMO=120        # per-invocation timeout

echo -e "engine\tmode\tws_bytes\tns_per_op\tinvocations\tchecksum" > "$OUT"

run_mode () { # mode ws_bytes
  local mode=$1 ws=$2
  local best="" bline=""
  for i in $(seq $K); do
    local line
    line=$(timeout $TMO "$DYNA" "$HERE/wp_cache_pressure.js" "$mode" "$ws" 2>/dev/null | grep '^RESULT ')
    if [ -z "$line" ]; then echo "WARN: no RESULT ($mode $ws) invocation $i" >&2; continue; fi
    local nsop
    nsop=${${(s: :)line}[4]}
    if [ -z "$best" ] || python3 -c "exit(0 if float('$nsop') < float('$best') else 1)" 2>/dev/null; then
      best=$nsop; bline=$line
    fi
  done
  if [ -n "$bline" ]; then
    local f1 f3 f4 f5
    f1=${${(s: :)bline}[2]}; f3=${${(s: :)bline}[4]}; f4=${${(s: :)bline}[5]}; f5=${${(s: :)bline}[6]}
    echo -e "engine\t$f1\t$f1-ws=$ws\t$f3\t$K\t$f5" >> "$OUT"
    echo "engine $f1 ws=$ws ns/op=$f3"
  fi
}

echo "== engine kernels ($K invocations each, min)" | tee -a "$OUT"
run_mode tight 0
run_mode broad 0
for kb in 4 16 64 256 1024 4096 16384; do
  ws=$((kb * 1024))
  run_mode f64read $ws
  run_mode f64rmw $ws
  run_mode objread $ws
done
for ws in 4096 16384 65536 262144 1048576; do
  run_mode mega $ws
done

echo "== C calibration bench" >> "$OUT"
cc -O2 -o "$HERE/calib_ws_bench" "$HERE/calib_ws_bench.c" || exit 1
timeout 300 "$HERE/calib_ws_bench" 2>/dev/null | while read -r line; do
  # line: RESULT <kind> <bytes> <ns> [(acc=...)]
  echo -e "calib\t${${(s: :)line}[2]}\t${${(s: :)line}[3]}\t${${(s: :)line}[4]}\t-\t-" >> "$OUT"
  echo "calib $line"
done

echo "== done, results in $OUT"
