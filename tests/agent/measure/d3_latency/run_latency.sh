#!/bin/zsh
# D3 interrupt-latency harness runner.
# Builds the harness against the tree's libdynajs.a (make must have run at
# least once), runs the interpreter classes (1000 interrupts each) and the
# giant-builtin classes (200 interrupts each), saving raw output + CSV.
#
# NOTE: builds with the libdynajs.a present in the tree; run on an IDLE
# machine -- the signal thread's timer and the handler timestamps are
# noise-sensitive in the tail (min/p50 are robust).
set -u
cd "$(dirname "$0")/../../../.."   # tree root (script lives 4 levels deep)
HERE=tests/agent/measure/d3_latency
OUT=$HERE/d3_results
mkdir -p "$OUT"

cc -g -O2 -I. -Isrc -o "$HERE/latency_harness" "$HERE/latency_harness.c" \
   libdynajs.a -lm -lpthread -ldl || exit 1

DYNA_BIN=${1:-./dynajs}
# Use the tree's engine binary only as a reference label; the harness embeds
# libdynajs.a directly (that IS the engine under test).
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
