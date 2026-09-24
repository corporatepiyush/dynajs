#!/bin/sh
# test_review_term.sh -- the terminal-module review corpus, as permanent
# regression rows. 9 harnesses / ~280 checks over the dyna:cli input surface:
# strict options bags, Command env/version/action precedence, StyleText
# emission matrices, Table refusals, the QuickJS teardown-leak detectors
# (the GC-list assertion at JS_FreeRuntime is the detector for the
# reference-leak class -- a leak sanitizer is blind to it), byte-exact key
# decoding under owned input timing, prompt/confirm stream alignment and the
# 1 MiB boundary, select() navigation, and real-pty rows for raw-mode
# restore and type-ahead survival. The python drivers SKIP when python3 is
# absent (that is the "portable" boundary); the JS rows always run.
#
# Usage: sh tests/test_review_term.sh [path-to-dynajs]
set -u
cd "$(dirname "$0")/.."
export DYNAJS="${1:-${DYNAJS:-./dynajs}}"
[ -x "$DYNAJS" ] || { echo "FAIL: binary $DYNAJS not found"; exit 1; }
sh tests/review_term/run_all.sh
