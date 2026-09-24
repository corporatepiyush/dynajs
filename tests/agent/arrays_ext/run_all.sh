#!/bin/zsh
# Runner for arrays_ext probes: dynajs vs node vs baseline, timeout-guarded.
# Usage: run_all.sh [glob-pattern]   (default: all *.js in this dir)
TREE="$(cd "$(dirname "$0")/../../.." && pwd)"
cd "$(dirname "$0")"
PAT="${1:-*.js}"
DYN="$TREE/dynajs"
BASE="$TREE/dynajs.base"
mkdir -p out
for f in $(ls ${~PAT} | grep -E '^[FSBRMX][0-9]+_'); do
  b="${f:r}"
  timeout 10 $DYN "$f" >"out/$b.dyna.txt" 2>&1; drc=$?
  timeout 20 node "$f" >"out/$b.node.txt" 2>&1; nrc=$?
  timeout 10 $BASE "$f" >"out/$b.base.txt" 2>&1; brc=$?
  verdict="PASS"
  [ $drc -ne 0 ] && verdict="FAIL-dyna-rc$drc"
  [ $nrc -ne 0 ] && verdict="$verdict node-rc$nrc"
  cmp -s "out/$b.dyna.txt" "out/$b.node.txt" || verdict="FAIL-DIFF-NODE"
  if [ $drc -eq 0 ] && [ $nrc -eq 0 ] && cmp -s "out/$b.dyna.txt" "out/$b.node.txt"; then
    if cmp -s "out/$b.dyna.txt" "out/$b.base.txt"; then bcls="same-as-baseline"; else bcls="differs-from-baseline"; fi
  else
    if cmp -s "out/$b.node.txt" "out/$b.base.txt" || (cmp -s "out/$b.dyna.txt" "out/$b.base.txt"); then bcls="pre-existing(mismatch-node,baseline-agrees-with-dyna)"; else bcls="CHECK-regression?(differs-from-both)"; fi
  fi
  echo "$b dyna-rc=$drc node-rc=$nrc $verdict baseline=$bcls"
done
