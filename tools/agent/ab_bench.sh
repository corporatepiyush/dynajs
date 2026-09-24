#!/usr/bin/env bash
# Interleaved same-tree A/B bench (AGENT.md): alternate $A and $B over the
# nursery kernel set, R reps each, report per-kernel medians + ratio + a
# byte-diff of kernel stdout (checksum probe) per binary.
# The kernel files live in bench/nursery/ which is GITIGNORED BY DESIGN
# (bench/* is local scratch), so this script PREFLIGHTS their existence and
# HARD-FAILS listing every missing file -- it must never fake-pass by
# comparing empty output to empty output.
# Usage: ab_bench.sh <binA=flag-off> <binB=flag-on> [reps]
cd "$(dirname "$0")/../.." || exit 2
A="${1:-./dynajs.base}"
B="${2:-./dynajs}"
R="${3:-3}"
[ -x "$A" ] || { echo "FAIL: $A missing or not executable"; exit 2; }
[ -x "$B" ] || { echo "FAIL: $B missing or not executable"; exit 2; }
KERNELS="bench/nursery/churn_vecmat.js bench/nursery/bench_ee.js bench/nursery/bench_json_records.js bench/nursery/controls.js"
missing=""
for k in $KERNELS; do
  [ -f "$k" ] || missing="$missing $k"
done
if [ -n "$missing" ]; then
  echo "FAIL: bench kernels missing (bench/* is not committed; they live in this tree only):"
  for k in $missing; do echo "  $k"; done
  exit 2
fi
mkdir -p bench/nursery/logs
for k in $KERNELS; do
  name=$(basename "$k" .js)
  for side in A B; do
    bin=$([ $side = A ] && echo "$A" || echo "$B")
    : > "bench/nursery/logs/$name.$side"
  done
  for rep in $(seq 1 "$R"); do
    for side in A B; do
      bin=$([ $side = A ] && echo "$A" || echo "$B")
      out=$(timeout 300 "$bin" "$k" 2>&1)
      rc=$?
      echo "$out" >> "bench/nursery/logs/$name.$side"
      echo "rep$rep rc=$rc" >> "bench/nursery/logs/$name.$side"
      sleep 0.1
    done
  done
done
echo "=== per-kernel results (median of $R reps; A=$(basename "$A") off, B=$(basename "$B") on) ==="
rc_total=0
for k in $KERNELS; do
  name=$(basename "$k" .js)
  a=$(grep " best " "bench/nursery/logs/$name.A" | sed 's/.*best \([0-9.]*\) ms.*/\1/' | sort -n | sed -n 2p)
  b=$(grep " best " "bench/nursery/logs/$name.B" | sed 's/.*best \([0-9.]*\) ms.*/\1/' | sort -n | sed -n 2p)
  # controls kernel prints one "name ms" line per control; compare line-wise
  ca=$(grep -E "checksum|sink" "bench/nursery/logs/$name.A" | tr '\n' ';')
  cb=$(grep -E "checksum|sink" "bench/nursery/logs/$name.B" | tr '\n' ';')
  match="MISMATCH"
  if [ "$ca" = "$cb" ]; then
    match="checksum-ok"
  fi
  if [ -z "$ca" ] || [ -z "$cb" ]; then
    match="FAIL-no-checksum-captured"
    rc_total=1
  elif [ "$ca" != "$cb" ]; then
    rc_total=1
  fi
  if [ -n "$a" ] && [ -n "$b" ]; then
    ratio=$(perl -e "printf '%.2fx', $a/$b" 2>/dev/null || echo "n/a")
  else
    ratio="n/a"
    echo "note: $name produced no parsable timing lines"
  fi
  printf "%-18s A=%-9s B=%-9s %s  %s\n" "$name" "$a" "$b" "$ratio" "$match"
  if [ "$name" = "controls" ]; then
    paste <(grep -E "^i_" "bench/nursery/logs/$name.A") <(grep -E "^i_" "bench/nursery/logs/$name.B") | awk '{printf "    %-22s %s vs %s\n", $1, $2, $5}'
  fi
done
exit $rc_total
