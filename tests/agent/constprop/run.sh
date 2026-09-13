#!/bin/zsh
# constprop probe runner: every probe must produce IDENTICAL output under
# node (oracle), ./dynajs (patched) and ./dynajs.base (pristine control).
# usage: tests/agent/constprop/run.sh [node|patched|base|all]
cd "$(dirname "$0")/../../.." || exit 1
MODE="${1:-all}"
pass=0; fail=0
run_one() {
    local f="$1" extra="$2"
    local nout d1 d2
    local nextra=""
    [[ "$f" == *.mjs ]] && nextra=""
    nout=$(node $nextra "$f" 2>&1)
    if [[ $MODE == node || $MODE == all ]]; then
        echo "--- $f (node oracle)"; echo "$nout"
    fi
    if [[ $MODE == patched || $MODE == all ]]; then
        d1=$(timeout 120 ./dynajs $extra "$f" 2>&1)
        if [[ "$d1" == "$nout" ]]; then pass=$((pass+1));
        else fail=$((fail+1)); echo "FAIL(patched) $f"; diff <(echo "$nout") <(echo "$d1") | head -10; fi
    fi
    if [[ $MODE == base || $MODE == all ]]; then
        if [[ -x ./dynajs.base ]]; then
            d2=$(timeout 120 ./dynajs.base $extra "$f" 2>&1)
            if [[ "$d2" == "$nout" ]]; then pass=$((pass+1));
            else fail=$((fail+1)); echo "FAIL(base) $f"; diff <(echo "$nout") <(echo "$d2") | head -10; fi
        fi
    fi
}
for f in tests/agent/constprop/tp*.js; do
    run_one "$f" ""
done
# module variant
run_one "tests/agent/constprop/tp17_module.mjs" "-m"
echo "=== constprop probes: pass=$pass fail=$fail"
[[ $fail == 0 ]]
