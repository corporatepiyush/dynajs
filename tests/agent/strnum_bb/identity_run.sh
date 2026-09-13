#!/bin/bash
# identity_run.sh — nursery-flag black-box identity check.
# Runs EVERY probe in probes/ (strings + dtoa) plus tests/agent/bbreview programs
# under dynajs.pristine AND dynajs.nursery; requires byte-identical stdout AND rc
# on every single one. Writes report to out_id/identity.tsv, exits nonzero on any
# difference.
set -u
cd "$(dirname "$0")"
ROOT=../../..
P="$ROOT/dynajs.pristine"
N="$ROOT/dynajs.nursery"
OUT=out_id
JOBS="${JOBS:-8}"
TMO="${TMO:-120}"
rm -rf "$OUT"
mkdir -p "$OUT/pristine" "$OUT/nursery"

pass1() { timeout "$TMO" "$1" "$2" > "$3" 2>/dev/null; echo $? > "$3.rc"; }
export -f pass1

find probes -name '*.js' | sort > "$OUT/probe.list"
N=$(wc -l < "$OUT/probe.list" | tr -d ' ')
echo "identity: $N probes x 2 binaries" >&2

cat "$OUT/probe.list" | xargs -P "$JOBS" -I{} bash -c 'pass1 "$0" "$1" "'"$OUT"'/pristine/$(basename "$1")" ' "$P" {} \;
cat "$OUT/probe.list" | xargs -P "$JOBS" -I{} bash -c 'pass1 "$0" "$1" "'"$OUT"'/nursery/$(basename "$1")" ' "$N" {} \;

# bbreview programs (both binaries, interleaved)
BB=../bbreview
ls "$BB"/*.js 2>/dev/null | sort > "$OUT/bb.list"
while read -r f; do
  b=$(basename "$f")
  ( timeout "$TMO" "$P" "$f" > "$OUT/pristine/bb_$b.txt" 2>/dev/null; echo $? > "$OUT/pristine/bb_$b.txt.rc" ) &
  ( timeout "$TMO" "$N" "$f" > "$OUT/nursery/bb_$b.txt" 2>/dev/null; echo $? > "$OUT/nursery/bb_$b.txt.rc" ) &
  while [ "$(jobs -r | wc -l | tr -d ' ')" -ge "$JOBS" ]; do wait -n 2>/dev/null || break; done
done < "$OUT/bb.list"
wait 2>/dev/null

# compare
: > "$OUT/identity.tsv"
echo -e "probe\tidentical_stdout\tidentical_rc" >> "$OUT/identity.tsv"
BAD=0
while read -r f; do
  b=$(basename "$f")
  cmp -s "$OUT/pristine/$b" "$OUT/nursery/$b"; s=$?
  cmp -s "$OUT/pristine/$b.rc" "$OUT/nursery/$b.rc"; r=$?
  echo -e "$b\t$([ $s -eq 0 ] && echo SAME || echo DIFF)\t$([ $r -eq 0 ] && echo SAME || echo DIFF)" >> "$OUT/identity.tsv"
  if [ $s -ne 0 ] || [ $r -ne 0 ]; then BAD=$((BAD+1)); fi
done < "$OUT/probe.list"
while read -r f; do
  b="bb_$(basename "$f")"
  cmp -s "$OUT/pristine/$b.txt" "$OUT/nursery/$b.txt"; s=$?
  cmp -s "$OUT/pristine/$b.txt.rc" "$OUT/nursery/$b.txt.rc"; r=$?
  echo -e "$b\t$([ $s -eq 0 ] && echo SAME || echo DIFF)\t$([ $r -eq 0 ] && echo SAME || echo DIFF)" >> "$OUT/identity.tsv"
  if [ $s -ne 0 ] || [ $r -ne 0 ]; then BAD=$((BAD+1)); fi
done < "$OUT/bb.list"
echo "identity: DIFFS=$BAD report=$OUT/identity.tsv" >&2
[ "$BAD" -eq 0 ] || exit 1
