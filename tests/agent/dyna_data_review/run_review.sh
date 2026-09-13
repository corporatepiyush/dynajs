#!/bin/bash
# run_review.sh [BIN] — run all review probes; default = patched ./dynajs
BIN="${1:-/Users/piyush/ai/dynascript/.agent-work/work44/dynajs}"
cd "$(dirname "$0")"
mkdir -p out
total_pass=0; total_fail=0; total_cases=0
for f in p*.js; do
  timeout 90 "$BIN" "$f" > "out/$f.txt" 2>&1
  rc=$?
  line=$(grep -m1 '^SUMMARY ' "out/$f.txt" || echo "SUMMARY $f none")
  p=$(echo "$line" | sed -n 's/.*pass=\([0-9]*\).*/\1/p'); fl=$(echo "$line" | sed -n 's/.*fail=\([0-9]*\).*/\1/p')
  c=$(echo "$line" | sed -n 's/.*cases=\([0-9]*\).*/\1/p')
  echo "$f rc=$rc $line"
  total_pass=$((total_pass+${p:-0})); total_fail=$((total_fail+${fl:-0})); total_cases=$((total_cases+${c:-0}))
done
echo "TOTAL cases=$total_cases pass=$total_pass fail=$total_fail"
