#!/bin/sh
# Build fff1.so — the test-only native module used by rv04_native_fff1.js
# (cproto JS_CFUNC_f_f_f with declared length 1: the exact arity shape that
# exercises js_call_c_function's unguarded arg_buf[1] read).
# Usage: sh tests/agent/method_fusion_v2/build_fff1.sh   (CWD = tree root)
# The engine's ASan variant needs the same for its sanitizer link:
#   make CONFIG_ASAN=y && sh tests/agent/method_fusion_v2/build_fff1.sh asan
set -eu
HERE=$(dirname "$0")
MODE=${1:-}
mkdir -p agent_debug .obj.rv

SANITY_FLAGS=""
if [ "$MODE" = "asan" ]; then
  SANITY_FLAGS="-fsanitize=address"
fi

clang -g -O2 -fwrapv -D_GNU_SOURCE -DCONFIG_VERSION=\"0.9.0\" \
  -DCONFIG_SYSTEMLIBM -I. -Isrc -DCONFIG_PROP_HASH_MIX -fPIC \
  -DJS_SHARED_LIBRARY $SANITY_FLAGS \
  -c -o .obj.rv/fff1.pic.o "$HERE/fff1.c"
clang -g -shared -undefined dynamic_lookup $SANITY_FLAGS \
  -o "$HERE/fff1.so" .obj.rv/fff1.pic.o -lm -lpthread -ldl
echo "fff1.so built ($MODE)"
