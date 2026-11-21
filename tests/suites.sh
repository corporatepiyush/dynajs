#!/usr/bin/env bash
# tests/suites.sh [DIR...] -- every JS suite the agent and the docker legs run: tests/**/test_*.js and bb_*.js,
# minus helpers (lib/), child/worker scripts (*_child.js, tests/known-unrun.txt), the optional-backend suites
# (sdl ui raylib uring: they need a build switch) and tests/agent (its own engine-comparison harness).
set -u
cd "$(dirname "$0")/.."
dirs=("$@"); [ $# -gt 0 ] || dirs=(tests)
find "${dirs[@]}" -name '*.js' \( -name 'test_*' -o -name 'bb_*' \) \
  | grep -v -e '/lib/' -e '_child\.js$' -e '^tests/sdl/' -e '^tests/ui/' -e '^tests/raylib/' -e '^tests/uring/' -e '^tests/agent/' \
  | grep -vxF -f <(grep -v '^#' tests/known-unrun.txt | cut -f1) | sort
