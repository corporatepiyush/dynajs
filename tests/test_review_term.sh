#!/bin/sh
set -u
cd "$(dirname "$0")/.."
export DYNAJS="${1:-${DYNAJS:-./dynajs}}"
[ -x "$DYNAJS" ] || { echo "FAIL: binary $DYNAJS not found"; exit 1; }
sh tests/review_term/run_all.sh
