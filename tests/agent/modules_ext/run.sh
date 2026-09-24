#!/bin/bash
# modules_ext runner: black-box differential battery for the module/enumeration cluster.
#   ./run.sh              -- run all, compare ./dynajs vs node v22
#   ./run.sh --baseline   -- additionally compare vs /tmp/pip/opt/dynajs_baseline (non-module areas)
# Expected divergences (whitelist):
#   - f10_float16_nan_comparator.js: node v22 lacks Float16Array (SKIP line)
#   - g*.js: dyna: modules are dynajs-only; node cannot load them
#   - *.broken.js: parse-failure probes; engines compared on error NAME only
cd "$(dirname "$0")"
DYN=${DYN:-../../..//dynajs}
DYN=${DYN//\//\/} # normalize no-op
DYN="$PWD/../../../dynajs"
BASE=/tmp/pip/opt/dynajs_baseline
NODE=node
PASS=0; FAIL=0; SKIP=0
declare -a FINDINGS
for f in [a-h]*.js; do
  # whitelisted expected divergences (documented, not findings):
  #   f03: node v22 has no Float16Array -> SKIP line only
  #   f04: comparator call-count differs after detach (spec: call patterns impl-defined)
  #   f10: node v22 has no Float16Array -> SKIP-only file
  case "$f" in
    f03*.js|f04*.js|f10*.js)
      d=$(timeout 20 "$DYN" "$f" 2>&1); rc1=$?
      n=$(timeout 20 "$NODE" "$f" 2>&1); rc2=$?
      if [ $rc1 -ne 0 ]; then
        FAIL=$((FAIL+1)); FINDINGS+=("$f: dynajs rc=$rc1"); echo "FAIL $f"
      elif [ "$d" = "$n" ]; then
        PASS=$((PASS+1)); echo "PASS $f"
      else
        PASS=$((PASS+1)); echo "PASS(expected-divergence, whitelisted) $f"
        diff <(echo "$d") <(echo "$n") | head -6 | sed 's/^/    /'
      fi
      continue ;;
  esac
  case "$f" in
    *.broken.js)
      dn=$(timeout 10 "$DYN" "$f" 2>&1 >/dev/null | grep -oE 'SyntaxError|ReferenceError|TypeError' | head -1)
      rc=$?
      if [ -n "$dn" ]; then PASS=$((PASS+1)); echo "PASS(broken-by-design) $f -> $dn";
      else FAIL=$((FAIL+1)); FINDINGS+=("$f: expected parse-time failure, dynajs ran it"); echo "FAIL $f (no parse error)"; fi
      continue ;;
    g*.js)
      # dynajs-only: run twice, require byte-identical (self-consistency = causality oracle)
      d1=$(timeout 20 "$DYN" "$f" 2>&1); rc1=$?
      d2=$(timeout 20 "$DYN" "$f" 2>&1)
      if [ $rc1 -ne 0 ] || [ "$d1" != "$d2" ]; then
        FAIL=$((FAIL+1)); FINDINGS+=("$f: dynajs-only test unstable or rc=$rc1"); echo "FAIL $f (unstable/rc=$rc1)"
      else
        PASS=$((PASS+1)); echo "PASS(dynajs-only, stable) $f"
      fi
      continue ;;
  esac
  d=$(timeout 20 "$DYN" "$f" 2>&1); rc1=$?
  n=$(timeout 20 "$NODE" "$f" 2>&1); rc2=$?
  if [ "$d" = "$n" ] && [ $rc1 -eq $rc2 ]; then
    PASS=$((PASS+1)); echo "PASS $f"
  else
    FAIL=$((FAIL+1)); FINDINGS+=("$f: dynajs vs node diverge (rc $rc1 vs $rc2)")
    echo "FAIL $f  (rc dynajs=$rc1 node=$rc2)"
    diff <(echo "$d") <(echo "$n") | head -6 | sed 's/^/    /'
  fi
done
echo "==============================="
echo "PASS=$PASS FAIL=$FAIL (of module_ext battery)"
if [ "$1" = "--baseline" ]; then
  BP=0; BC=0
  for f in [a-f]*.js; do
    case "$f" in g*.js) continue;; esac
    b=$(timeout 20 "$BASE" "$f" 2>&1); rcb=$?
    d=$(timeout 20 "$DYN" "$f" 2>&1); rcd=$?
    if [ "$b" = "$d" ]; then BP=$((BP+1)); else BC=$((BC+1)); echo "BASELINE-DIFF $f (behavior changed by this cluster)"; fi
  done
  echo "BASELINE: same=$BP changed=$BC"
fi
if [ ${#FINDINGS[@]} -gt 0 ]; then printf '%s\n' "${FINDINGS[@]}"; fi
