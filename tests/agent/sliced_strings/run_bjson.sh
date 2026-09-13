#!/bin/sh
# bjson round-trip probe (engine-only): builds a module INSIDE tests/ with a
# relative import (the engine rejects absolute .so module names) and runs it.
d=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$d/../../.." && pwd)
E=$(cd "$(dirname "$1")" 2>/dev/null && pwd)/$(basename "$1")
tmp="$ROOT/tests/.bjson_$$"
mkdir -p "$tmp"
{
  echo 'import * as bjson from "../bjson.so";'
  cat "$d/_h.js"
  cat "$d/21_bjson_roundtrip.js"
} > "$tmp/21.mjs"
cd "$ROOT/tests"
timeout 60 "$E" -m ".bjson_$$/21.mjs"
rc=$?
cd - > /dev/null
rm -rf "$tmp"
exit $rc
