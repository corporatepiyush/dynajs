#!/bin/bash
# run.sh [PROBE_DIR] — run every probe under BOTH engines (dynajs + node oracle),
# timeout-wrapped, parallel. Per-probe stdout -> out/dynajs/<probe>.txt and
# out/node/<probe>.txt (rc in sibling .rc files). Emits summary.tsv.
# Exits nonzero if any probe fails on either engine (or any engine crashes).
# Env overrides: DYNAJS_BIN, NODE_BIN, OUTDIR (for identity runs), JOBS, TMO.
set -u
cd "$(dirname "$0")"
PROBE_DIR="${1:-probes/strings}"
ROOT=../../..
DYNAJS_BIN="${DYNAJS_BIN:-$ROOT/dynajs}"
NODE_BIN="${NODE_BIN:-node}"
OUTDIR="${OUTDIR:-out}"
JOBS="${JOBS:-8}"
TMO="${TMO:-60}"
mkdir -p "$OUTDIR/dynajs" "$OUTDIR/node"

LIST=$(find "$PROBE_DIR" -name '*.js' | sort)
N=$(echo "$LIST" | wc -l | tr -d ' ')
echo "run.sh: $N probes, probe_dir=$PROBE_DIR, dynajs=$DYNAJS_BIN node=$NODE_BIN" >&2

echo "$LIST" | xargs -P "$JOBS" -I{} bash run_one.sh {} dynajs "$DYNAJS_BIN" "$OUTDIR/dynajs" "$TMO"
echo "$LIST" | xargs -P "$JOBS" -I{} bash run_one.sh {} node "$NODE_BIN" "$OUTDIR/node" "$TMO"

TSV="$OUTDIR/summary.tsv"
{
  echo -e "probe\tengine\trc\tpass\tfail\tresult"
  for b in $(echo "$LIST" | xargs -n1 basename | sed 's/\.js$//'); do
    for eng in dynajs node; do
      txt="$OUTDIR/$eng/$b.txt"; rc="$OUTDIR/$eng/$b.rc"
      if [ ! -f "$txt" ]; then echo -e "$b\t$eng\tMISSING\t0\t0\tCRASH"; continue; fi
      r=$(echo "$rc" | cat 2>/dev/null)
      summ=$(grep -m1 '^SUMMARY ' "$txt" 2>/dev/null || echo "SUMMARY none pass=0 fail=0")
      pass=$(echo "$summ" | sed -n 's/.*pass=\([0-9]*\).*/\1/p')
      fail=$(echo "$summ" | sed -n 's/.*fail=\([0-9]*\).*/\1/p')
      res=$(grep -m1 '^RESULT ' "$txt" 2>/dev/null | awk '{print $2}')
      [ -z "$pass" ] && pass=0; [ -z "$fail" ] && fail=0
      [ -z "$res" ] && res=NORESULT
      rcv=$(cat "$rc" 2>/dev/null || echo 999)
      echo -e "$b\t$eng\t$rcv\t$pass\t$fail\t$res"
    done
  done
} > "$TSV"

BAD=$(awk -F'\t' 'NR>1 && ($3!=0 || $6!="PASS")' "$TSV" | wc -l | tr -d ' ')
echo "run.sh: done. failures(rc!=0 or result!=PASS): $BAD  summary: $TSV" >&2
if [ "$BAD" -ne 0 ]; then
  awk -F'\t' 'NR>1 && ($3!=0 || $6!="PASS")' "$TSV" | head -50 >&2
  exit 1
fi
exit 0
