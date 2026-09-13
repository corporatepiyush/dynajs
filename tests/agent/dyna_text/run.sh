#!/bin/bash
# run.sh [FAMILY|all] — run the dyna_text probe battery.
#
# The dyna:* modules only load on dynajs, so the engine under test is dynajs;
# the ORACLE is python3 stdlib baked into the probes at materialize time (see
# materialize.sh). Per-probe stdout+rc go to SEPARATE out/<family>/ files plus
# a summary.tsv. Exits nonzero if any probe fails, times out (rc=124), or
# crashes (no RESULT line).
#
# Env overrides: DYNAJS_BIN, OUTDIR (default out), JOBS, TMO.
set -u
cd "$(dirname "$0")"
FAMILY="${1:-all}"
ROOT=../../..
DYNAJS_BIN="${DYNAJS_BIN:-$ROOT/dynajs}"
OUTDIR="${OUTDIR:-out}"
JOBS="${JOBS:-8}"
TMO="${TMO:-120}"
mkdir -p "$OUTDIR/dynajs"

# scratch space probes may write into (CSV fixtures etc.); keep out of /tmp
mkdir -p "$OUTDIR/scratch"

if [ "$FAMILY" = "all" ]; then
  LIST=$(find probes -name '*.js' | sort)
else
  LIST=$(find "probes/$FAMILY" -name '*.js' 2>/dev/null | sort)
fi
N=$(echo "$LIST" | grep -c . || true)
echo "run.sh: $N probes, family=$FAMILY, dynajs=$DYNAJS_BIN" >&2
if [ "$N" = "0" ]; then echo "run.sh: no probes; run ./materialize.sh first" >&2; exit 2; fi

echo "$LIST" | xargs -P "$JOBS" -I{} bash run_one.sh {} dynajs "$DYNAJS_BIN" "$OUTDIR/dynajs" "$TMO"

TSV="$OUTDIR/summary.tsv"
{
  echo -e "probe\tengine\trc\tpass\tfail\tresult"
  for b in $(echo "$LIST" | xargs -n1 basename | sed 's/\.js$//'); do
    eng=dynajs
    txt="$OUTDIR/$eng/$b.txt"; rc="$OUTDIR/$eng/$b.rc"
    if [ ! -f "$txt" ]; then echo -e "$b\t$eng\tMISSING\t0\t0\tCRASH"; continue; fi
    summ=$(grep -m1 '^SUMMARY ' "$txt" 2>/dev/null || echo "SUMMARY none pass=0 fail=0")
    pass=$(echo "$summ" | sed -n 's/.*pass=\([0-9]*\).*/\1/p')
    fail=$(echo "$summ" | sed -n 's/.*fail=\([0-9]*\).*/\1/p')
    res=$(grep -m1 '^RESULT ' "$txt" 2>/dev/null | awk '{print $2}')
    [ -z "$pass" ] && pass=0; [ -z "$fail" ] && fail=0
    [ -z "$res" ] && res=NORESULT
    rcv=$(cat "$rc" 2>/dev/null || echo 999)
    echo -e "$b\t$eng\t$rcv\t$pass\t$fail\t$res"
  done
} > "$TSV"

BAD=$(awk -F'\t' 'NR>1 && ($3!=0 || $6!="PASS")' "$TSV" | wc -l | tr -d ' ')
TOT_PASS=$(awk -F'\t' 'NR>1 {s+=$4} END {print s+0}' "$TSV")
TOT_FAIL=$(awk -F'\t' 'NR>1 {s+=$5} END {print s+0}' "$TSV")
echo "run.sh: done. probes=$N failures(rc!=0 or result!=PASS)=$BAD asserts pass=$TOT_PASS fail=$TOT_FAIL  summary: $TSV" >&2
if [ "$BAD" -ne 0 ]; then
  awk -F'\t' 'NR>1 && ($3!=0 || $6!="PASS")' "$TSV" | head -60 >&2
  exit 1
fi
exit 0
