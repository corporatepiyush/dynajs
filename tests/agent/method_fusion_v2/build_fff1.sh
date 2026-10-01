#!/bin/sh
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
