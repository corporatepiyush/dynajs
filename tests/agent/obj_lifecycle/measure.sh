#!/bin/sh
# Interleaved min-of-7 measurement: pristine vs candidate for each kernel.
# usage: sh measure.sh <candidate-binary> [tag]
cd "$(dirname "$0")/../../.." || exit 1
CAND="${1:-./dynajs}"
TAG="${2:-cand}"
PRI=./dynajs.pristine
RUNS=7
KERNELS="churn_vec2 ev_emit json_records"

for k in $KERNELS; do
  echo "== $k =="
  for b in PRI CAND; do
    if [ "$b" = PRI ]; then B=$PRI; else B=$CAND; fi
    best=""
    n=0
    while [ $n -lt $RUNS ]; do
      t=$("$B" tests/agent/obj_lifecycle/$k.js | sed 's/.*time=\([0-9]*\)ms.*/\1/')
      if [ -z "$best" ] || [ "$t" -lt "$best" ]; then best=$t; fi
      n=$((n+1))
    done
    if [ "$b" = PRI ]; then PRI_T=$best; else CAND_T=$best; fi
  done
  echo "$k pri=$PRI_T cand=$CAND_T speedup=$(echo "scale=4; $PRI_T / $CAND_T" | bc)"
done
