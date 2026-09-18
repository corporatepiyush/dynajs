#!/bin/sh
# Differential breadth runner: dynajs vs node over the work44 bbreview probe
# corpus (copied read-only reference) + the method-fusion battery.
# Usage: run_diff.sh BINARY CORPUS_DIR [OUTDIR]   (CWD = tree root)
set -u
BIN=$1
CORPUS=$2
OUT=${3:-agent_debug/diff_out}
mkdir -p "$OUT"
fail=0
n=0
HARNESS="$(dirname "$0")/bbreview_h.js"
for f in "$CORPUS"/*.js; do
  b=$(basename "$f")
  case "$b" in timing_probe.js) continue;; esac
  n=$(( n + 1 ))
  case "$b" in e0*|e1*) TZV=America/New_York;; *) TZV=UTC;; esac
  cat "$HARNESS" "$f" > "$OUT/$b.comb.js"
  TZ=$TZV LC_ALL=C timeout 120 "$BIN" "$OUT/$b.comb.js" > "$OUT/$b.dyna" 2>&1; drc=$?
  TZ=$TZV LC_ALL=C timeout 120 node "$OUT/$b.comb.js" > "$OUT/$b.node" 2>&1; nrc=$?
  if [ $drc -ge 128 ] || [ $drc -eq 139 ]; then
    echo "CRASH $b rc=$drc"; fail=$(( fail + 1 )); continue
  fi
  if ! cmp -s "$OUT/$b.dyna" "$OUT/$b.node" || [ $drc -ne $nrc ]; then
    echo "DIFF $b (rc $drc/$nrc)"; fail=$(( fail + 1 ))
  fi
done
echo "$CORPUS: $n files, $fail failures"
exit $fail
