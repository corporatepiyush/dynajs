#!/bin/bash
set -u
cd "$(dirname "$0")/../../.."
ROOT=$(pwd)
SRC=tests/agent/async_exit_fixes/fix1_stale_cache_body_close.js
DIR=scratch/stale_cache
FAIL=0

rm -rf "$DIR"; mkdir -p "$DIR"
cp "$SRC" "$DIR/p.js"

out=$(DYNAJS_BYTECODE_CACHE=1 timeout 30 ./dynajs.pristine "$DIR/p.js" 2>&1)
rc=$?
[ "$rc" = 0 ] && [ "$out" = "ret=1" ] || { echo "FAIL step2 (pristine baseline): rc=$rc out='$out'"; FAIL=1; }
[ -f "$DIR/p.js.qbc" ] || { echo "FAIL step2: no .qbc written"; FAIL=1; }

cp "$DIR/p.js.qbc" "$DIR/p.pristine.qbc"
vb=$(dd if="$DIR/p.js.qbc" bs=1 skip=48 count=1 2>/dev/null | od -An -tu1 | tr -d ' ')
[ "$vb" = "12" ] || { echo "FAIL step2: expected pristine blob BC_VERSION 12, got '$vb'"; FAIL=1; }

out=$(DYNAJS_BYTECODE_CACHE=1 timeout 30 ./dynajs "$DIR/p.js" 2>&1)
rc=$?
[ "$rc" = 0 ] && [ "$out" = "ret=1" ] || { echo "FAIL step3 (patched semantics): rc=$rc out='$out'"; FAIL=1; }

vb2=$(dd if="$DIR/p.js.qbc" bs=1 skip=48 count=1 2>/dev/null | od -An -tu1 | tr -d ' ')
[ "$vb2" = "13" ] || { echo "FAIL step4: cache not refreshed (blob BC_VERSION '$vb2', want 13)"; FAIL=1; }
if cmp -s "$DIR/p.pristine.qbc" "$DIR/p.js.qbc"; then
    echo "FAIL step4: .qbc byte-identical to the pristine blob (cache was EXECUTED, not recompiled)"; FAIL=1
fi

if [ "$FAIL" = 0 ]; then
    echo "RESULT PASS"
else
    echo "RESULT FAIL"
fi
exit $FAIL
