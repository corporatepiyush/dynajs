#!/bin/sh
# Census: prove the OP2_call_method* fused ops actually FIRE in the benchmark
# kernels and in the equivalence corpus. Builds a throwaway dump-enabled binary
# (DUMP_BYTECODE=1) into agent_debug/ and counts opcodes in the pass-3 dumps.
# Usage: sh tests/agent/method_fusion/census.sh   (CWD = tree root)
set -u
HERE=$(dirname "$0")
DBG=agent_debug
mkdir -p "$DBG"

DUMPBIN="$DBG/dynajsc.dump_mf"
FORCE=${1:-}
if [ ! -x "$DUMPBIN" ] || [ "$FORCE" = "--force" ]; then
  echo "building dump binary ($DUMPBIN)..."
  mkdir -p .obj.dump
  clang -g -O1 -std=gnu17 -fwrapv -D_GNU_SOURCE -DCONFIG_VERSION='"0.9.0"' \
    -DCONFIG_SYSTEMLIBM -I. -Isrc -DCONFIG_PROP_HASH_MIX -DDUMP_BYTECODE=1 \
    -c -o .obj.dump/dynajs.o src/dynajs.c || exit 1
  clang -g -rdynamic -o "$DUMPBIN" .obj.dump/dynajs.o .obj/dynajsc.o \
    .obj/dtoa.o .obj/libregexp.o .obj/libunicode.o .obj/cutils.o \
    .obj/dyna-libc.o .obj/dyna-io.o .obj/dyn-hash.o .obj/dyn-codec.o \
    .obj/dyn-prng.o .obj/dyn-compress.o .obj/dyn-ds.o .obj/dyn-serial.o \
    .obj/dyn-path.o .obj/dyn-mathx.o .obj/dyn-ac.o .obj/dyn-dict.o \
    .obj/dyn-pool.o .obj/dyn-timer.o .obj/dyn-dns.o .obj/dyn-resp.o \
    .obj/dyn-scram.o .obj/dyn-snappy.o .obj/dyna-simd-core.o \
    .obj/dyna-simd-scalar.o .obj/dyna-simd-neon.o .obj/dyna-simd-sse42.o \
    .obj/dyna-simd-avx2.o .obj/dyna-simd-avx512.o .obj/dyna-simd-sve.o \
    .obj/repl.o -lm -lpthread -ldl || exit 1
fi

fail=0
count_ops() { # file, pattern
  "$DUMPBIN" -c -o /dev/null "$1" 2>/dev/null | grep -c "$2"
}

echo "== census: fused ops per kernel =="
for f in "$HERE"/mf_arg_forms.js "$HERE"/mf_tasks.js "$HERE"/mf_edges.js \
         "$HERE"/mf_qbc_body.js; do
  n0=$(count_ops "$f" "call_method0")
  n1=$(count_ops "$f" "call_method1_loc")
  n2=$(count_ops "$f" "call_method1_imm8")
  n3=$(count_ops "$f" "call_method1_const")
  total=$((n0 + n1 + n2 + n3))
  echo "  $(basename "$f"): method0=$n0 method1_loc=$n1 method1_imm8=$n2 method1_const=$n3"
  if [ $total -eq 0 ]; then echo "  NO FUSED OPS in $(basename "$f")"; fail=1; fi
done

# kernels used for A/B: the fused op must fire in each.
cat > "$DBG/mf_kernel_cc.js" <<'EOF'
function sumcc(s, n) { var h = 0; for (var i = 0; i < n; i++) h += s.charCodeAt(i); return h; }
var s = ""; for (var i = 0; i < 64; i++) s += String.fromCharCode(32 + (i % 95));
var t = 0; for (var r = 0; r < 5000; r++) t += sumcc(s, 64);
console.log(t);
EOF
cat > "$DBG/mf_kernel_math.js" <<'EOF'
function kmath(n) { var a = 0; for (var i = 1; i <= n; i++) a += Math.sqrt(i); return a; }
var t = 0; for (var r = 0; r < 20000; r++) t += kmath(8);
console.log(t);
EOF
# push_const (u32 cpool) form: needs a cpool index >= 256 -> 300 closures first
gen="$DBG/mf_kernel_const.js"
{
  echo 'function kconst(s) {'
  i=0
  while [ $i -lt 300 ]; do
    echo "  var f$i = function(){ return $i; }; f$i();"
    i=$((i+1))
  done
  echo '  var r = s.charCodeAt(2.5);'
  echo '  return r;'
  echo '}'
  echo 'console.log(kconst("abc"));'
} > "$gen"
for k in "$DBG/mf_kernel_cc.js" "$DBG/mf_kernel_math.js"; do
  n=$(count_ops "$k" "call_method1")
  echo "  $(basename "$k"): fused=$n"
  if [ "$n" -eq 0 ]; then echo "  NO FUSED OPS in kernel"; fail=1; fi
done
gen="$DBG/mf_kernel_const.js"
n=$(count_ops "$gen" "call_method1_const")
timeout 60 ./dynajs "$gen" > "$DBG/mf_kernel_const.dyna.out" 2>&1
timeout 60 node "$gen" > "$DBG/mf_kernel_const.node.out" 2>&1
if ! cmp -s "$DBG/mf_kernel_const.dyna.out" "$DBG/mf_kernel_const.node.out"; then
  echo "  mf_kernel_const: OUTPUT DIFF vs node"; fail=1
fi
echo "  mf_kernel_const.js: method1_const=$n (output matches node)"
if [ "$n" -eq 0 ]; then echo "  NO method1_const in kernel"; fail=1; fi

if [ $fail -eq 0 ]; then echo "census: fused ops verified"; else echo "census: MISSING FUSED OPS"; fi
exit $fail
