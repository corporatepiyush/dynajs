#!/bin/zsh
# interleaved A/B: patched vs pristine, N reps, prints each rep wall time (ms)
f="$1"; reps="${2:-5}"
for i in $(seq $reps); do
  a=$( { /usr/bin/time -p ./dynajs.patched "$f" >/dev/null; } 2>&1 | awk '/real/{print int($2*1000)}' )
  b=$( { /usr/bin/time -p ./dynajs.pristine "$f" >/dev/null; } 2>&1 | awk '/real/{print int($2*1000)}' )
  echo "rep$i patched=$a pristine=$b"
done
