#!/bin/sh
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
