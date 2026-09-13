#!/bin/zsh
# bbreview matrix, in-tree: dynajs vs node (primary oracle) with the two
# documented base-binding exceptions; timeout-guarded.
TREE="$(cd "$(dirname "$0")/../../.." && pwd)"
cd "$(dirname "$0")"
DYN="$TREE/dynajs"
BASE="$TREE/dynajs.base"
mkdir -p out
pass=0; fail=0; basebind=0
for f in *.js; do
  case "$f" in
    e06_date_pre1900.js|g02_snan_abs.js) MODE=base ;;
    *) MODE=node ;;
  esac
  case "$f" in e0*|e1*) TZ=America/New_York;; *) TZ=UTC;; esac
  export TZ
  timeout 60 $DYN "$f" > "out/${f:r}.dyna.txt" 2>&1; drc=$?
  if [ "$MODE" = node ]; then
    timeout 60 node "$f" > "out/${f:r}.node.txt" 2>&1; nrc=$?
    if [ $drc -eq $nrc ] && cmp -s "out/${f:r}.dyna.txt" "out/${f:r}.node.txt"; then
      pass=$((pass+1))
    else
      echo "FAIL $f (dyna=$drc node=$nrc)"; fail=$((fail+1))
    fi
  else
    timeout 60 $BASE "$f" > "out/${f:r}.base.txt" 2>&1; brc=$?
    if [ $drc -eq $brc ] && cmp -s "out/${f:r}.dyna.txt" "out/${f:r}.base.txt"; then
      basebind=$((basebind+1))
    else
      echo "FAIL $f (base-binding: dyna=$drc base=$brc)"; fail=$((fail+1))
    fi
  fi
done
echo "bbreview: pass=$pass basebind=$basebind fail=$fail"
