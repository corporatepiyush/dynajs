#!/bin/bash
# run.sh [PROBE_DIR] — run every probe under dynajs (the engine under test).
# Probes whose filename starts with `n_` are pure-JS (no native imports) and
# ALSO run under node as a cross-engine oracle; their outputs land in
# out/node/*.txt. Per-probe stdout -> out/dynajs/<probe>.txt (rc in .rc).
# Emits summary.tsv; exits nonzero on any failure.
# Env overrides: DYNAJS_BIN, NODE_BIN, OUTDIR, JOBS, TMO.
set -u
cd "$(dirname "$0")"
PROBE_DIR="${1:-probes}"
ROOT=../../..
DYNAJS_BIN="${DYNAJS_BIN:-$ROOT/dynajs}"
NODE_BIN="${NODE_BIN:-node}"
OUTDIR="${OUTDIR:-out}"
JOBS="${JOBS:-6}"
TMO="${TMO:-90}"
export TZ="${TZ:-UTC}"   # pinned so localtime-dependent probes are deterministic
mkdir -p "$OUTDIR/dynajs" "$OUTDIR/node"

LIST=$(find "$PROBE_DIR" -name '*.js' | sort)
N=$(echo "$LIST" | wc -l | tr -d ' ')
echo "run.sh: $N probes, probe_dir=$PROBE_DIR, dynajs=$DYNAJS_BIN node=$NODE_BIN TZ=$TZ" >&2

echo "$LIST" | xargs -P "$JOBS" -I{} bash run_one.sh {} dynajs "$DYNAJS_BIN" "$OUTDIR/dynajs" "$TMO"
N_LIST=$(echo "$LIST" | (grep '/n_' || true))
N_N=$(echo "$N_LIST" | grep -c . || true)
echo "run.sh: $N_N node-oracle probes" >&2
[ -n "$N_LIST" ] && echo "$N_LIST" | xargs -P "$JOBS" -I{} bash run_one.sh {} node "$NODE_BIN" "$OUTDIR/node" "$TMO"

TSV="$OUTDIR/summary.tsv"
{
  echo -e "probe\tengine\trc\tpass\tfail\tresult"
  for p in $LIST; do
    b=$(basename "$p" .js)
    for eng in dynajs node; do
      # node column only for n_ probes
      case "$b" in n_*) ;; *) if [ "$eng" = node ]; then continue; fi ;; esac
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
  done
} > "$TSV"

BAD=$(awk -F'\t' 'NR>1 && ($3!=0 || $6!="PASS")' "$TSV" | wc -l | tr -d ' ')
echo "run.sh: done. failures(rc!=0 or result!=PASS): $BAD  summary: $TSV" >&2
if [ "$BAD" -ne 0 ]; then
  awk -F'\t' 'NR>1 && ($3!=0 || $6!="PASS")' "$TSV" | head -60 >&2
  exit 1
fi
exit 0
