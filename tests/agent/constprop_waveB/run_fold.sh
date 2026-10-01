#!/bin/zsh
cd "$(dirname "$0")/../../.." || exit 1
pass=0; fail=0
OUT=tests/agent/constprop_waveB/out
mkdir -p "$OUT"
for f in tests/agent/constprop_waveB/fold/fold_*.js; do
    b=$(basename "$f")
    exp="tests/agent/constprop_waveB/expected/$b.txt"
    [ -f "$exp" ] || { echo "NO-EXPECTED $b"; fail=$((fail+1)); continue; }
    ok=1
    node "$f" > "$OUT/node_$b.txt" 2>&1 || ok=0
    timeout 60 ./dynajs "$f" > "$OUT/dynajs_$b.txt" 2>&1 || ok=0
    timeout 60 ./dynajs.base "$f" > "$OUT/base_$b.txt" 2>&1 || ok=0
    [[ $ok == 1 ]] || { echo "RC-FAIL $b"; fail=$((fail+1)); continue; }
    for e in node dynajs base; do
        if ! LC_ALL=C cmp -s "$exp" "$OUT/${e}_$b.txt"; then
            echo "DIFF($e) $b"
            diff "$exp" "$OUT/${e}_$b.txt" | head -6
            fail=$((fail+1)); ok=0; break
        fi
    done
    [[ $ok == 1 ]] && pass=$((pass+1))
done
echo "=== fold-active probes: pass=$pass fail=$fail"
[[ $fail == 0 ]]
