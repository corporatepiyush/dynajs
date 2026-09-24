#!/bin/bash
# run.sh [PROBE_DIR] — run every probe under dynajs (the only engine that can
# load dyna:* modules; the golden oracle is python decimal/numpy baked into the
# probes at generation time). Timeout-wrapped, parallel, per-probe stdout ->
# out/dynajs/<probe>.txt (rc in sibling .rc files). Emits summary.tsv.
# Exits nonzero if any probe fails or crashes.
# Env overrides: DYNAJS_BIN, OUTDIR, JOBS, TMO.
set -u
cd "$(dirname "$0")"
PROBE_DIR="${1:-probes}"
ROOT=../../..
DYNAJS_BIN="${DYNAJS_BIN:-$ROOT/dynajs}"
OUTDIR="${OUTDIR:-out}"
JOBS="${JOBS:-8}"
TMO="${TMO:-120}"
rm -rf "$OUTDIR/dynajs"
mkdir -p "$OUTDIR/dynajs"

LIST=$(find "$PROBE_DIR" -name '*.js' | sort)
N=$(echo "$LIST" | wc -l | tr -d ' ')
echo "run.sh: $N probes, probe_dir=$PROBE_DIR, dynajs=$DYNAJS_BIN" >&2

echo "$LIST" | xargs -P "$JOBS" -I{} bash run_one.sh {} dynajs "$DYNAJS_BIN" "$OUTDIR/dynajs" "$TMO"

TSV="$OUTDIR/summary.tsv"
{
  echo -e "probe\trc\tpass\tfail\tresult"
  for b in $(echo "$LIST" | xargs -n1 basename | sed 's/\.js$//'); do
    txt="$OUTDIR/dynajs/$b.txt"; rc="$OUTDIR/dynajs/$b.rc"
    if [ ! -f "$txt" ]; then echo -e "$b\tMISSING\t0\t0\tCRASH"; continue; fi
    summ=$(grep -m1 '^SUMMARY ' "$txt" 2>/dev/null || echo "SUMMARY none pass=0 fail=0")
    pass=$(echo "$summ" | sed -n 's/.*pass=\([0-9]*\).*/\1/p')
    fail=$(echo "$summ" | sed -n 's/.*fail=\([0-9]*\).*/\1/p')
    res=$(grep -m1 '^RESULT ' "$txt" 2>/dev/null | awk '{print $2}')
    [ -z "$pass" ] && pass=0; [ -z "$fail" ] && fail=0
    [ -z "$res" ] && res=NORESULT
    rcv=$(cat "$rc" 2>/dev/null || echo 999)
    echo -e "$b\t$rcv\t$pass\t$fail\t$res"
  done
} > "$TSV"
cat "$TSV"

# nonzero exit on ANY failure: fail>0, missing RESULT, or engine crash (rc>1 —
# the probe itself exits 1 via the thrown harness error, which is already a fail)
BAD=$(awk -F'\t' 'NR>1 && ($5 != "PASS") {print $1}' "$TSV")
if [ -n "$BAD" ]; then
  echo "run.sh: FAILING PROBES:" >&2
  echo "$BAD" >&2
  exit 1
fi
echo "run.sh: ALL $N PROBES PASS" >&2
