#!/bin/zsh
# ASan battery over the x2/x3 async + closure batteries (plus x1/x5/x6 probes).
# Usage: asan_battery.sh [asan-binary]   (default .obj/asan/dynajs)
TREE="$(cd "$(dirname "$0")/../../.." && pwd)"
DYN="${1:-$TREE/.obj/asan/dynajs}"
cd "$TREE"
fails=0
run() {
  local f="$1"
  # detect_leaks=0: the engine has a pre-existing ~272-byte exit leak even on
  # `console.log(1)` (also present on the pristine baseline) - we gate on
  # memory ERRORS (UAF/overflow/UB), not exit-time allocations.
  ASAN_OPTIONS=detect_leaks=0 timeout 60 "$DYN" "$f" > "$TREE/tests/agent/defect_fixes/asan_out.txt" 2> "$TREE/tests/agent/defect_fixes/asan_err.txt"
  local rc=$?
  if [ $rc -ne 0 ] && [ $rc -ne 1 ]; then echo "RC-FAIL($rc) $f"; fails=$((fails+1)); return; fi
  if grep -qE "ERROR: AddressSanitizer|runtime error" "$TREE/tests/agent/defect_fixes/asan_err.txt"; then
    echo "ASAN-FAIL $f"; head -6 "$TREE/tests/agent/defect_fixes/asan_err.txt"; fails=$((fails+1)); return
  fi
  echo "ok $f"
}
for f in tests/agent/parser_core_ext/x2_loop_binding_labeled_continue.js \
         tests/agent/parser_core_ext/x2b_per_iteration_shapes.js \
         tests/agent/parser_core_ext/x3_ag_forawait_close_order.js \
         tests/agent/parser_core_ext/x3b_forawait_close_shapes.js \
         tests/agent/parser_core_ext/x4_ag_throw_drain_hang.js \
         tests/agent/parser_core_ext/a01_ag_multi_consumers.js \
         tests/agent/parser_core_ext/a02_ag_consumer_throws.js \
         tests/agent/parser_core_ext/a03_ag_return_during_next.js \
         tests/agent/parser_core_ext/x1b_let_func_order_matrix.js \
         tests/agent/parser_core_ext/x1c_let_func_order_edges.js \
         tests/agent/defect_fixes/x2_shapes.js \
         tests/agent/defect_fixes/x3_shapes.js \
         tests/agent/defect_fixes/x5_shapes.js \
         tests/agent/defect_fixes/x6_shapes.js \
         tests/agent/defect_fixes/x1_matrix_eval.js; do
  run "$f"
done
echo "asan battery: fails=$fails"
