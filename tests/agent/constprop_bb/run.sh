#!/bin/bash
# run.sh — full 3-way matrix run: patched (./dynajs) vs pristine (pre-feature) vs node.
# Exits nonzero if any probe has a non-OK verdict.
set -u
cd "$(dirname "$0")"
rm -rf out
mkdir -p out/dynajs out/pristine out/node out/rows
touch ../../../heartbeat

N=$(awk 'NR>1 {print $1}' manifest.tsv)
JOBS="${JOBS:-8}"

printf '%s\n' $N | xargs -P "$JOBS" -I{} ./runner_one.sh {} &
XPID=$!
# heartbeat while the run goes
( while kill -0 "$XPID" 2>/dev/null; do touch ../../../heartbeat; sleep 60; done ) &
HPID=$!
wait "$XPID"; RC=$?
kill "$HPID" 2>/dev/null

{
  echo -e "pid\tmode\tdeclined\tpatched_fail\tpristine_fail\tnode_fail\tbyte_eq\tverdict"
  cat out/rows/*.tsv | sort
} > summary.tsv

total=$(($(wc -l < summary.tsv) - 1))
ok=$(    awk -F'\t' 'NR>1 && $8=="OK"' summary.tsv | wc -l | tr -d ' ')
reg=$(   awk -F'\t' 'NR>1 && $8=="REGRESSION"' summary.tsv | wc -l | tr -d ' ')
snd=$(   awk -F'\t' 'NR>1 && $8=="SOUNDNESS"' summary.tsv | wc -l | tr -d ' ')
pre=$(   awk -F'\t' 'NR>1 && $8=="PRE-EXISTING"' summary.tsv | wc -l | tr -d ' ')
fix=$(   awk -F'\t' 'NR>1 && $8=="FIXED-VS-PRISTINE"' summary.tsv | wc -l | tr -d ' ')
bd=$(    awk -F'\t' 'NR>1 && $8=="BYTE-DIFF"' summary.tsv | wc -l | tr -d ' ')
bde=$(   awk -F'\t' 'NR>1 && $8=="BYTE-DIFF-ELIGIBLE"' summary.tsv | wc -l | tr -d ' ')
gen=$(   awk -F'\t' 'NR>1 && $8=="GENBUG-NODEFAIL"' summary.tsv | wc -l | tr -d ' ')
p3=$(    awk -F'\t' 'NR>1 && $8=="OK" && $7==1' summary.tsv | wc -l | tr -d ' ')

echo "== summary: total=$total OK=$ok REGRESSION=$reg SOUNDNESS=$snd PRE-EXISTING=$pre FIXED=$fix BYTE-DIFF=$bd BYTE-DIFF-ELIGIBLE=$bde GENBUG=$gen"
echo "== 3-way byte-identical (patched==pristine bytes && all pass): $p3 / $total"
awk -F'\t' 'NR>1 && $8!="OK" {print}' summary.tsv > failures.tsv
if [ "$RC" != "0" ] || [ "$total" -eq 0 ] || [ "$ok" -ne "$total" ]; then
  echo "RUN FAILED (runner rc=$RC, ok=$ok/$total); see failures.tsv"
  exit 1
fi
echo "RUN PASSED"
