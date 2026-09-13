#!/bin/bash
# run.sh [PATTERN] — run dyna_sys probes against the in-tree dynajs build.
# Per-probe stdout+rc to SEPARATE files under out/<engine>/, plus summary.tsv.
# Probes that need a control server each get their OWN server on an EPHEMERAL
# port (never two probes on a fixed port), so independent probes parallelize.
# Exits nonzero if any probe fails.
set -u
cd "$(dirname "$0")"
PATTERN="${1:-}"
DYNAJS_BIN="${DYNAJS_BIN:-../../../dynajs}"
PRISTINE_BIN="${PRISTINE_BIN:-../../../dynajs.pristine}"
OUTDIR="${OUTDIR:-out}"
JOBS="${JOBS:-6}"
TMO="${TMO:-90}"
mkdir -p "$OUTDIR/dynajs" scratch

LIST=$(find probes -name '*.js' ! -name '*_child.js' | sort)
if [ -n "$PATTERN" ]; then LIST=$(echo "$LIST" | grep -E "$PATTERN" || true); fi
N=$(echo "$LIST" | grep -c . || true)
echo "run.sh: $N probes, dynajs=$DYNAJS_BIN jobs=$JOBS tmo=$TMO" >&2
[ "$N" -eq 0 ] && { echo "run.sh: no probes matched" >&2; exit 2; }

echo "$LIST" | xargs -P "$JOBS" -I{} bash run_one.sh {} dynajs "$DYNAJS_BIN" "$OUTDIR/dynajs" "$TMO"

TSV="$OUTDIR/summary.tsv"
{
  echo -e "probe\trc\tpass\tfail\tresult"
  for f in $(echo "$LIST" | xargs -n1 basename | sed 's/\.js$//'); do
    txt="$OUTDIR/dynajs/$f.txt"; rc="$OUTDIR/dynajs/$f.rc"
    if [ ! -f "$txt" ]; then echo -e "$f\tMISSING\t0\t0\tCRASH"; continue; fi
    summ=$(grep -m1 '^SUMMARY ' "$txt" 2>/dev/null || echo "SUMMARY none pass=0 fail=0")
    pass=$(echo "$summ" | sed -n 's/.*pass=\([0-9]*\).*/\1/p'); [ -z "$pass" ] && pass=0
    fail=$(echo "$summ" | sed -n 's/.*fail=\([0-9]*\).*/\1/p'); [ -z "$fail" ] && fail=0
    res=$(grep -m1 '^RESULT ' "$txt" 2>/dev/null | awk '{print $2}'); [ -z "$res" ] && res=NORESULT
    rcv=$(cat "$rc" 2>/dev/null || echo 999)
    echo -e "$f\t$rcv\t$pass\t$fail\t$res"
  done
} > "$TSV"

BAD=$(awk -F'\t' 'NR>1 && ($2!=0 || $5!="PASS")' "$TSV" | wc -l | tr -d ' ')
echo "run.sh: done. failures(rc!=0 or result!=PASS): $BAD  summary: $TSV" >&2
if [ "$BAD" -ne 0 ]; then
  awk -F'\t' 'NR>1 && ($2!=0 || $5!="PASS")' "$TSV" | head -60 >&2
  exit 1
fi
exit 0
