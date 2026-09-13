#!/bin/sh
# bench runner: interleaved A/B, REPS reps per engine, min ns/op wins.
# usage: sh run_bench.sh <engineA=pristine> <engineB=patched> [bench.js ...]
# output: <bench>.tsv in cwd with per-case min ns/op for each engine + ratio.
set -u
A=${1:?engineA}; B=${2:?engineB}; shift 2
d=$(cd "$(dirname "$0")" && pwd)
REPS=${REPS:-5}
files="$*"
[ $# -gt 0 ] || files="$d"/b[0-9]*.js
for f in $files; do
  b=$(basename "$f" .js)
  tmp="$d/.b_$b.js"
  cat "$d/_b.js" "$f" > "$tmp"
  rm -f "$d/.res_$b.A" "$d/.res_$b.B"
  r=0
  while [ $r -lt $REPS ]; do
    timeout 300 "$A" "$tmp" 2>/dev/null | grep '^BENCH ' | sed "s/^BENCH /$r /" >> "$d/.res_$b.A"
    timeout 300 "$B" "$tmp" 2>/dev/null | grep '^BENCH ' | sed "s/^BENCH /$r /" >> "$d/.res_$b.B"
    r=$((r+1))
  done
  {
    echo -e "case\tpristine_ns\tpatched_ns\tratio"
    awk '{ k=$2; v=$3; if (!(k in m) || v < m[k]) m[k]=v } END { for (k in m) print k "\t" m[k] }' "$d/.res_$b.A" | sort > "$d/.min_$b.A"
    awk '{ k=$2; v=$3; if (!(k in m) || v < m[k]) m[k]=v } END { for (k in m) print k "\t" m[k] }' "$d/.res_$b.B" | sort > "$d/.min_$b.B"
    join -t"$(printf '\t')" "$d/.min_$b.A" "$d/.min_$b.B" | awk -F'\t' '{ printf "%s\t%s\t%s\t%.2f\n", $1, $2, $3, ($2>0)?$2/$3:0 }'
  } > "$d/$b.tsv"
  rm -f "$d/.res_$b.A" "$d/.res_$b.B" "$d/.min_$b.A" "$d/.min_$b.B" "$tmp"
  echo "== $b =="; cat "$d/$b.tsv"
done
