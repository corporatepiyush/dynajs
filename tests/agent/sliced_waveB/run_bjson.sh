#!/bin/sh
# sliced_waveB bjson round-trip probe (engine-only): builds a module INSIDE
# tests/ with a relative import (the engine rejects absolute .so module
# names) and runs the 09 probe as an .mjs. Same mechanism as the wave-A
# suite's run_bjson.sh. Requires tests/bjson.so (make test builds it).
d=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$d/../../.." && pwd)
E=$(cd "$(dirname "$1")" 2>/dev/null && pwd)/$(basename "$1")
tmp="$ROOT/tests/.bjson_w58_$$"
mkdir -p "$tmp"
{
  echo 'import * as bjson from "../bjson.so";'
  cat "$ROOT/tests/agent/_h/h.js"
  cat "$d/_h.js"
  cat "$d/09_bjson_atom_slice.js"
} > "$tmp/09_bjson_atom_slice.mjs"
cd "$ROOT/tests" || exit 2
timeout 60 "$E" -m ".bjson_w58_$$/09_bjson_atom_slice.mjs"
rc=$?
cd - > /dev/null
rm -rf "$tmp"
exit $rc
