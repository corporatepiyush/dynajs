#!/bin/sh
# 3-way pristine/ON/OFF perf protocol: order-rotated, min-of-7, in-process
# best-of (the kernels do the in-process part; this driver does min-of-7 and
# rotation). Prints the medians/ratios table.
# Usage: sh tests/agent/method_fusion_v2/perf3way.sh [ROUNDS] [BIN3...]
#   BIN3 defaults to: ./dynajs.pristine ./dynajs.on ./dynajs.off
#   (first entry is the baseline "pristine")
set -eu
HERE=$(dirname "$0")
ROUNDS=${1:-7}
shift 2>/dev/null || true
if [ $# -ge 3 ]; then BINP="$1"; BINON="$2"; BINOFF="$3";
else
  BINP=./dynajs.pristine; BINON=./dynajs.on; BINOFF=./dynajs.off
fi
BINX=${BINX:-}; BINX2=${BINX2:-}
K="$HERE/perf_kernels.js"

names="cc_loop method_loc method_arg sqrt_loop empty_loop call2_ctrl task_scan task_churn"

rotate() { # echo rotation r of the binary list
  r=$1
  set -- $BINP $BINON $BINOFF
  [ -n "$BINX" ] && set -- "$@" "$BINX"
  [ -n "$BINX2" ] && set -- "$@" "$BINX2"
  n=$#
  s=$(( r % n ))
  i=0
  for b in "$@"; do
    if [ $i -ge $s ]; then echo "$b"; fi
    i=$(( i + 1 ))
  done
  i=0
  for b in "$@"; do
    if [ $i -lt $s ]; then echo "$b"; fi
    i=$(( i + 1 ))
  done
}

# results: per binary per kernel min ns — flat files in agent_debug/p3way
OUT=agent_debug/p3way
rm -rf "$OUT"; mkdir -p "$OUT"

r=0
while [ $r -lt $ROUNDS ]; do
  r=$(( r + 1 ))
  order=$(rotate $r)
  for b in $order; do
    tag=$(echo "$b" | tr './' '__')
    sh "$HERE/run_one.sh" "$b" "$K" "$OUT/$tag.samples" || { echo "RUN-FAIL $b"; exit 1; }
  done
done

# fold samples into minima
for b in $BINP $BINON $BINOFF; do
  [ -n "$BINX" ] && b="$b $BINX"
  : # loop below handles empties
done
for s in "$OUT"/*.samples; do
  tag=$(basename "$s" .samples)
  {
    echo "$tag"
    for k in $names; do
      v=$(grep "^$k	" "$s" | cut -f2 | sort -n | head -1)
      echo "$k	$v"
    done
  } > "$OUT/$tag.min"
done

echo "=== min-of-$ROUNDS (ns per rep; lower is better) ==="
printf "%-12s %12s %12s %12s" kernel pristine on off
[ -n "$BINX" ] && printf " %12s" xbin
[ -n "$BINX2" ] && printf " %12s" xbin2
echo
for k in $names; do
  p=$(grep "^$k	" "$OUT"/*pristine.min | head -1 | cut -f2)
  o=$(grep "^$k	" "$OUT"/*on.min | head -1 | cut -f2)
  f=$(grep "^$k	" "$OUT"/*off.min | head -1 | cut -f2)
  printf "%-12s %12s %12s %12s" "$k" "$p" "$o" "$f"
  if [ -n "$BINX" ]; then
    xtag=$(echo "$BINX" | tr './' '__')
    x=$(grep "^$k	" "$OUT/$xtag.min" | head -1 | cut -f2)
    printf " %12s" "$x"
  fi
  if [ -n "$BINX2" ]; then
    x2tag=$(echo "$BINX2" | tr './' '__')
    x2=$(grep "^$k	" "$OUT/$x2tag.min" | head -1 | cut -f2)
    printf " %12s" "$x2"
  fi
  echo
done
echo "=== ratios (baseline = pristine; 1.000 = parity) ==="
for k in $names; do
  p=$(grep "^$k	" "$OUT"/*pristine.min | head -1 | cut -f2)
  o=$(grep "^$k	" "$OUT"/*on.min | head -1 | cut -f2)
  f=$(grep "^$k	" "$OUT"/*off.min | head -1 | cut -f2)
  line=$(printf "%-12s ON %s OFF %s" "$k" \
    "$(awk -v a="$o" -v b="$p" 'BEGIN{printf "%.3f", a/b}')" \
    "$(awk -v a="$f" -v b="$p" 'BEGIN{printf "%.3f", a/b}')")
  if [ -n "$BINX" ]; then
    xtag=$(echo "$BINX" | tr './' '__')
    x=$(grep "^$k	" "$OUT/$xtag.min" | head -1 | cut -f2)
    [ -n "$x" ] && line="$line T3 $(awk -v a="$x" -v b="$p" 'BEGIN{printf "%.3f", a/b}')"
  fi
  if [ -n "$BINX2" ]; then
    x2tag=$(echo "$BINX2" | tr './' '__')
    x2=$(grep "^$k	" "$OUT/$x2tag.min" | head -1 | cut -f2)
    [ -n "$x2" ] && line="$line X2 $(awk -v a="$x2" -v b="$p" 'BEGIN{printf "%.3f", a/b}')"
  fi
  echo "$line"
done
