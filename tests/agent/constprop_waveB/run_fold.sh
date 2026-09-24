#!/bin/zsh
# run_fold.sh — fold-active layer of the constprop wave B matrix.
#
# The unified runner (tests/agent/_h/run.sh) prepends the shared harness,
# which makes every probe decl non-first-statement: correctness is still
# checked there, but the fold paths are (deliberately, soundly) declined.
# This runner executes the fold/fold_*.js variants STANDALONE — decl-first
# holds, so B1/B2/B3 folds are active — and requires the transcript of
# every engine to equal the node-baked expected/ file exactly:
#   node (oracle), ./dynajs (patched), ./dynajs.base (wave A control),
#   ./dynajs.pristine (pristine control, when present).
# usage: tests/agent/constprop_waveB/run_fold.sh
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
