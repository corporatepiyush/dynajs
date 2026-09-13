#!/bin/zsh
# classification: X = patched-vs-pristine divergence (patch effect)
#   X-same       -> patch inert (PASS)
#   X-diff, node==pristine -> REGRESSION (patch broke spec behavior)
#   X-diff, node!=pristine  -> pre-existing gap zone; patched==node means gap closed
cd "$(dirname "$0")/../../.." || exit 1
pass=0; reg=0; inert_gap=0; closed=0
for f in tests/agent/constprop_review/rp([0-9]*)\.js; do
  nout=$(timeout 60 node "$f" 2>&1); nrc=$?
  dout=$(timeout 60 ./dynajs "$f" 2>&1); drc=$?
  bout=$(timeout 60 ./dynajs.pristine "$f" 2>&1); brc=$?
  if [[ "$dout" == "$bout" && $drc == $brc ]]; then pass=$((pass+1)); continue; fi
  if [[ "$bout" == "$nout" && $brc == $nrc ]]; then
    reg=$((reg+1)); echo "REGRESSION $f"; diff <(echo "$nout") <(echo "$dout") | head -6
  elif [[ "$dout" == "$nout" && $drc == $nrc ]]; then
    closed=$((closed+1)); echo "GAP-CLOSED $f (patched matches node, pristine did not)"
  else
    inert_gap=$((inert_gap+1)); echo "GAP-ZONE $f (patched differs from pristine; neither matches node rc)"; echo "  node:"; echo "$nout" | head -3 | sed 's/^/    /'; echo "  patched:"; echo "$dout" | head -3 | sed 's/^/    /'; echo "  pristine:"; echo "$bout" | head -3 | sed 's/^/    /'
  fi
done
echo "=== review: patch-inert=$pass REGRESSION=$reg gap-closed=$closed gap-zone=$inert_gap"
