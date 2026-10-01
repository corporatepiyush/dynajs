#!/usr/bin/env bash
set -u
cd "$(dirname "$0")"
MATRICES="${@:-x1 x2 x3 x5 x6}"

for m in $MATRICES; do
  if [ ! -f "probes/$m/manifest.tsv" ]; then
    echo "[run] generating $m..."
    python3 "gen_$m.py" > "gen_$m.log" 2>&1 || { echo "generator $m failed"; tail -5 "gen_$m.log"; exit 2; }
  fi
done

rm -rf out/node out/dynajs out/pristine
mkdir -p out/node out/dynajs out/pristine
: > probe_jobs.txt
for m in $MATRICES; do
  cut -f1 "probes/$m/manifest.tsv" | while read -r pid; do
    printf '%s %s %s\n' dynajs "$m" "$pid"
    printf '%s %s %s\n' node "$m" "$pid"
    printf '%s %s %s\n' pristine "$m" "$pid"
  done >> probe_jobs.txt
done
TOTAL=$(wc -l < probe_jobs.txt | tr -d ' ')
echo "[run] $TOTAL engine-runs starting ($(date -u +%H:%M:%S))"
xargs -P 8 -L 1 bash run_one.sh < probe_jobs.txt
echo "[run] engine-runs done ($(date -u +%H:%M:%S))"

python3 classify.py
rc=$?
echo "[run] classify rc=$rc"
exit $rc
