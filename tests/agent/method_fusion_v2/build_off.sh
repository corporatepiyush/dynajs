#!/bin/sh
# Build the CONFIG_FUSED_METHOD=0 oracle binary (dynajs.off): identical source
# tree, only the fusion gate flipped. Mirrors census.sh's single-unit recompile
# so the OFF binary differs from ON by exactly one compiled unit.
# Usage: sh tests/agent/method_fusion_v2/build_off.sh   (CWD = tree root)
set -eu
DBG=agent_debug
mkdir -p "$DBG" .obj.off

OBJLIST=".obj/dyna-cli.o .obj/repl.o .obj/dtoa.o .obj/libregexp.o .obj/libunicode.o \
.obj/cutils.o .obj/dyna-libc.o .obj/dyna-io.o .obj/dyn-hash.o .obj/dyn-codec.o \
.obj/dyn-prng.o .obj/dyn-compress.o .obj/dyn-ds.o .obj/dyn-serial.o \
.obj/dyn-path.o .obj/dyn-mathx.o .obj/dyn-ac.o .obj/dyn-dict.o .obj/dyn-pool.o \
.obj/dyn-timer.o .obj/dyn-dns.o .obj/dyn-resp.o .obj/dyn-scram.o \
.obj/dyn-snappy.o .obj/dyna-simd-core.o .obj/dyna-simd-scalar.o \
.obj/dyna-simd-neon.o .obj/dyna-simd-sse42.o .obj/dyna-simd-avx2.o \
.obj/dyna-simd-avx512.o .obj/dyna-simd-sve.o"

# recompile if any compiled-in source is newer than the off object
# (dynajs.c includes parser/interpreter/serialize units — a -nt check on
# dynajs.c alone misses edits to the included files)
if [ src/dynajs.c -nt .obj.off/dynajs.o ] || \
   [ src/vm/interpreter.inc.c -nt .obj.off/dynajs.o ] || \
   [ src/parser/parser.inc.c -nt .obj.off/dynajs.o ] || \
   [ src/serialize/bc_write.inc.c -nt .obj.off/dynajs.o ] || \
   [ ! -x "$DBG/../dynajs.off" ]; then
  clang -g -Wall -MMD -MF .obj.off/dynajs.o.d -Wextra -Wno-sign-compare \
    -Wno-missing-field-initializers -Wundef -Wuninitialized -Wunused \
    -Wno-unused-parameter -Wwrite-strings -Wchar-subscripts -funsigned-char \
    -std=gnu17 -fwrapv -D_GNU_SOURCE -DCONFIG_VERSION=\"0.9.0\" \
    -DCONFIG_SYSTEMLIBM -I. -Isrc -DCONFIG_PROP_HASH_MIX -O2 \
    -DCONFIG_FUSED_METHOD=0 \
    -c -o .obj.off/dynajs.o src/dynajs.c
  clang -g -rdynamic -o dynajs.off .obj.off/dynajs.o $OBJLIST -lm -lpthread -ldl
  echo "dynajs.off built"
else
  echo "dynajs.off up to date"
fi
