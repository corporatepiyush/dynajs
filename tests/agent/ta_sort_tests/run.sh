#!/bin/sh
# Regression runner for typed-array sort NaN/comparator semantics.
# Usage: tests/agent/ta_sort_tests/run.sh [path-to-engine]
#   engine defaults to ./dynajs at repo root.
# Gate 1: assert test passes under the engine.
# Gate 2 (if the pristine control binary exists): the 15-combo differential
#   driver output must be byte-identical to the control (exact
#   revert-to-pre-15f3b24 semantics; see diff_combos.js).
set -e
cd "$(dirname "$0")/../../.."
ENGINE="${1:-./dynajs}"
CONTROL=/tmp/pip/opt/dynajs_baseline

echo "== assert test under $ENGINE"
"$ENGINE" tests/agent/ta_sort_tests/test_ta_sort_nan_comparator.js

echo "== differential vs pristine control"
"$ENGINE" tests/agent/ta_sort_tests/diff_combos.js > tests/agent/ta_sort_tests/out_patched.txt
if [ -x "$CONTROL" ]; then
  "$CONTROL" tests/agent/ta_sort_tests/diff_combos.js > tests/agent/ta_sort_tests/out_control.txt
  diff tests/agent/ta_sort_tests/out_control.txt tests/agent/ta_sort_tests/out_patched.txt
  echo "CONTROL: byte-identical"
else
  echo "control binary not found at $CONTROL -- skipping differential (golden: out_control.txt)"
  diff tests/agent/ta_sort_tests/out_control.txt tests/agent/ta_sort_tests/out_patched.txt
  echo "GOLDEN: byte-identical"
fi
echo "OK"
