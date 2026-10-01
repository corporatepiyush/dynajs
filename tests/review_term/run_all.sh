#!/usr/bin/env bash
set -u
cd "$(dirname "$0")/../.."
export DYNAJS="${DYNAJS:-./dynajs}"
rc=0
if command -v timeout >/dev/null 2>&1; then TO="timeout 120"; else TO=""; fi
run() { echo "=== $*"; $TO "$@" </dev/null || { echo "!! PROBE RUNNER FAILED: $* (rc=$?)"; rc=1; }; }
run "$DYNAJS" --std tests/review_term/probe_bags.js
run "$DYNAJS" --std tests/review_term/probe_cmd.js
run "$DYNAJS" --std tests/review_term/probe_style.js
run "$DYNAJS" --std tests/review_term/probe_table.js
run "$DYNAJS" --std tests/review_term/probe_leak.js
if command -v python3 >/dev/null 2>&1; then
  run python3 tests/review_term/keys_driver.py
  run python3 tests/review_term/line_driver.py
  run python3 tests/review_term/select_driver.py
  run python3 tests/review_term/probe_pty.py
else
  echo "=== SKIP keys/line/select/pty drivers: python3 not found"
fi
echo "=== run_all exit $rc"
exit $rc
