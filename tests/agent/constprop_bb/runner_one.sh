#!/bin/bash
cd "$(dirname "$0")"
PID="$1"
MODE=$(awk -F'\t' -v p="$PID" '$1==p {print $3}' manifest.tsv)
DEC=$(awk -F'\t' -v p="$PID" '$1==p {print $4}' manifest.tsv)
P="probes/$PID.js"
TMO="${TMO:-10}"

TIMEOUT_BIN=$(command -v timeout || command -v gtimeout)

run_engine() {
  local dir="$1"; shift
  local out start_rc
  "$TIMEOUT_BIN" "$TMO" "$@" > "out/$dir/$PID.txt" 2>&1
  echo "rc=$?" >> "out/$dir/$PID.txt"
}

if [ "$MODE" = "module" ]; then
  run_engine dynajs   ../../../dynajs -m "$P"
  run_engine pristine ../../../dynajs.prefeature -m "$P"
else
  run_engine dynajs   ../../../dynajs "$P"
  run_engine pristine ../../../dynajs.prefeature "$P"
fi

pass_of() {
  local f="out/$1/$PID.txt"
  local rc line
  rc=$(grep -E '^rc=-?[0-9]+' "$f" | tail -1 | sed 's/rc=//')
  line=$(grep '^SUMMARY ' "$f" | tail -1)
  if [ "$rc" = "0" ] && echo "$line" | grep -q 'fail=0'; then echo 0; else echo 1; fi
}

if [ "$MODE" = "module" ]; then
  "$TIMEOUT_BIN" "$TMO" node --input-type=module < "$P" > "out/node/$PID.txt" 2>&1
  echo "rc=$?" >> "out/node/$PID.txt"
else
  run_engine node node "$P"
fi

PD=$(pass_of dynajs); PR=$(pass_of pristine); ND=$(pass_of node)
BYT=$(cmp -s "out/dynajs/$PID.txt" "out/pristine/$PID.txt" && echo 1 || echo 0)

verdict="OK"
if [ "$ND" = "1" ]; then verdict="GENBUG-NODEFAIL"
elif [ "$PD" = "1" ] && [ "$PR" = "0" ]; then
  if [ "$DEC" = "1" ]; then verdict="SOUNDNESS"; else verdict="REGRESSION"; fi
elif [ "$PD" = "1" ] && [ "$PR" = "1" ]; then verdict="PRE-EXISTING"
elif [ "$PD" = "0" ] && [ "$PR" = "1" ]; then verdict="FIXED-VS-PRISTINE"
elif [ "$BYT" = "0" ]; then
  if [ "$DEC" = "1" ]; then verdict="BYTE-DIFF"; else verdict="BYTE-DIFF-ELIGIBLE"; fi
fi

printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$PID" "$MODE" "$DEC" "$PD" "$PR" "$ND" "$BYT" "$verdict" > "out/rows/$PID.tsv"
