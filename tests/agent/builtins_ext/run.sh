#!/bin/bash
set -u
cd "$(dirname "$0")"
OUT=results
rm -rf "$OUT"; mkdir -p "$OUT"
FRAGDIR=fragments
rm -rf "$FRAGDIR"; mkdir -p "$FRAGDIR"
export FRAGDIR

PARALLEL=1
if [ "${1:-}" = "-s" ]; then PARALLEL=0; fi

UNITLIST="$OUT/units.txt"
: > "$UNITLIST"
for f in test_*.js self_*.js perf_*.js; do
  for tzi in 0 1 2; do
    echo "$f $tzi" >> "$UNITLIST"
  done
done

T_START=$(date +%s)
if [ "$PARALLEL" = 1 ]; then
  xargs -P 8 -L 1 bash run_one.sh < "$UNITLIST" > /dev/null
else
  while read -r f tzi; do bash run_one.sh "$f" "$tzi" > /dev/null; done < "$UNITLIST"
fi
T_END=$(date +%s)

total=0; pass=0; other=0
FAILLIST=()
MERGED=$(mktemp)
for frag in "$FRAGDIR"/frag.*; do
  cat "$frag"
done | sort > "$MERGED"
while IFS= read -r line; do
  [ -z "$line" ] && continue
  total=$((total+1))
  v=$(printf '%s' "$line" | awk '{print $1}')
  if [ "$v" = PASS ]; then
    pass=$((pass+1))
    printf '%s\n' "$line"
  else
    other=$((other+1))
    FAILLIST+=("$line")
  fi
done < "$MERGED"
rm -f "$MERGED"
echo "=================================================================="
echo "runs=$total PASS=$pass DIFF/CRASH=$other  (wall $((T_END-T_START))s, parallel=$PARALLEL)"
if [ ${#FAILLIST[@]} -gt 0 ]; then
  echo "--- non-PASS list:"
  for l in "${FAILLIST[@]}"; do echo "  $l"; done
fi
