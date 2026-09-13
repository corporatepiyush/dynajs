#!/bin/bash
# fix1_stale_cache_probe.sh — F1 regression probe: a .qbc written by the
# PRE-PATCH binary must be REJECTED (recompiled) by the patched binary, not
# executed. Executing it would silently skip the for-await iterator close on
# body throws (the loop's catch marker stays -1: the stale bytecode has no
# OP_for_await_catch_body restore).
#
# Steps:
#   1. stage the payload into scratch/stale_cache/
#   2. pristine + DYNAJS_BYTECODE_CACHE=1: runs it (prints ret=1) and writes
#      the pre-patch .qbc
#   3. patched + DYNAJS_BYTECODE_CACHE=1: with the fix, the cfg_hash
#      (opcode-table signature) mismatches -> miss -> recompile -> ret=1.
#      Without the fix: header hit -> stale blob -> ret=0 -> this probe FAILS.
#   4. the .qbc must now carry the patched blob (BC_VERSION byte 13, and the
#      cfg_hash no longer equals the pristine key).
set -u
cd "$(dirname "$0")/../../.."      # -> tree root
ROOT=$(pwd)
SRC=tests/agent/async_exit_fixes/fix1_stale_cache_body_close.js
DIR=scratch/stale_cache
FAIL=0

rm -rf "$DIR"; mkdir -p "$DIR"
cp "$SRC" "$DIR/p.js"

# 2) pristine compiles and caches
out=$(DYNAJS_BYTECODE_CACHE=1 timeout 30 ./dynajs.pristine "$DIR/p.js" 2>&1)
rc=$?
[ "$rc" = 0 ] && [ "$out" = "ret=1" ] || { echo "FAIL step2 (pristine baseline): rc=$rc out='$out'"; FAIL=1; }
[ -f "$DIR/p.js.qbc" ] || { echo "FAIL step2: no .qbc written"; FAIL=1; }

cp "$DIR/p.js.qbc" "$DIR/p.pristine.qbc"
# blob starts right after the 48-byte QbcHeader; first byte = BC_VERSION
vb=$(dd if="$DIR/p.js.qbc" bs=1 skip=48 count=1 2>/dev/null | od -An -tu1 | tr -d ' ')
[ "$vb" = "12" ] || { echo "FAIL step2: expected pristine blob BC_VERSION 12, got '$vb'"; FAIL=1; }

# 3) patched run: must MISS (cfg_hash opcode signature), recompile, execute correctly
out=$(DYNAJS_BYTECODE_CACHE=1 timeout 30 ./dynajs "$DIR/p.js" 2>&1)
rc=$?
[ "$rc" = 0 ] && [ "$out" = "ret=1" ] || { echo "FAIL step3 (patched semantics): rc=$rc out='$out'"; FAIL=1; }

# 4) the cache entry was replaced with the patched blob (BC_VERSION 13) and
#    its cfg_hash differs from the pristine key
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
