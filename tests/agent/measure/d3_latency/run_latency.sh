#!/bin/zsh
set -u
cd "$(dirname "$0")/../../../.."
HERE=tests/agent/measure/d3_latency
OUT=$HERE/d3_results
mkdir -p "$OUT"

cc -g -O2 -I. -Isrc -o "$HERE/latency_harness" "$HERE/latency_harness.c" \
   libdynajs.a -lm -lpthread -ldl || exit 1

DYNA_BIN=${1:-./dynajs}
echo "== harness engine: $DYNA_BIN vs $(ls -la libdynajs.a | awk '{print $5}') byte libdynajs.a"

timeout 300 "$HERE/latency_harness" --nogiant 2>"$OUT/latency_interp.err" \
   | tee "$OUT/latency_interp.txt"
DYNA_LAT_CSV="$OUT/latency_interp.csv" timeout 300 \
   "$HERE/latency_harness" --nogiant >/dev/null 2>&1 || true

timeout 600 "$HERE/latency_harness" --giant 2>"$OUT/latency_giant.err" \
   | tee "$OUT/latency_giant.txt"
DYNA_LAT_CSV="$OUT/latency_giant.csv" timeout 600 \
   "$HERE/latency_harness" --giant >/dev/null 2>&1 || true

echo "== done: $OUT"
