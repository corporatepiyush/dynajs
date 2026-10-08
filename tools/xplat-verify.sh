#!/usr/bin/env bash
# xplat-verify.sh -- cross-platform (amd64 Linux) COMPATIBILITY gate.
#
# Why this exists, and what it is NOT for:
#
#   On an arm64 Mac, `docker --platform linux/amd64` runs under qemu. MEASURED:
#   the same property read is 2.4 ns native and 85 ns emulated -- ~35x inflation.
#   So this script NEVER reports a timing as a result. What it DOES check is the
#   half that emulation does not distort:
#
#     * COMPATIBILITY  -- does it build and pass the suites on x86-64/glibc?
#     * CORRECTNESS    -- do the x86 SIMD kernels (SSE4.2/AVX2), which NEVER
#                         execute on the arm64 dev host, produce byte-identical
#                         output to the arm64/NEON run?
#     * MEMORY         -- engine malloc accounting is architecture-real; the two
#                         platforms must agree (measured: within 0.7%).
#
#   A differential-oracle test is exactly the right thing to run here: its output
#   is a SHA, and a SHA is emulation-independent.
#
# Usage:  tools/xplat-verify.sh [--build] [test.js ...]
#         --build   rebuild the image first (needed after any source change)
#
# Exit nonzero on any mismatch. Terse output, like build.sh.
set -uo pipefail
cd "$(dirname "$0")/.." || exit 2

IMAGE=dynajs:xplat
DOCKERFILE=docker/Dockerfile
DOCKER_TARGET=glibc
[ "${1:-}" = "--build" ] && shift

TESTS=("$@")
[ ${#TESTS[@]} -eq 0 ] && TESTS=(tests/test_regexp_prefilter.js tests/test_dataframe.js
                                 tests/test_object_literal_presize.js
                                 tests/oracle_string_indexof.js
                                 tests/test_matcher.js
                                 tests/oracle_compress_bytes.js
                                 tests/test_dictionary.js
                                 tests/test_bytes_handle.js
                                 tests/test_hash_split.js
                                 tests/test_crypto.js
                                 tests/test_ml_boosting.js
                                 tests/test_iterator_lazy.js
                                 tests/oracle_ml_hist.js
                                 tests/test_ml_xgb.js
                                 tests/test_ml_sparse.js
                                 tests/test_ml_weights.js
                                 tests/test_ext_batch8.js
                                 tests/test_time_dateparser.js
                                 tests/test_encoding.js)

fail=0
say(){ printf '%-46s %s\n' "$1" "$2"; }
die(){ echo "FAIL: $*" >&2; exit 1; }

command -v docker >/dev/null 2>&1 || die "docker not found"
docker info >/dev/null 2>&1 || die "docker daemon not running"

if true; then
  echo "building $IMAGE (linux/amd64, emulated -- cached layers keep this fast"
  echo "  when the source has not moved; slow the first time)..."
  docker build --pull --platform linux/amd64 --target "$DOCKER_TARGET" \
    -f "$DOCKERFILE" -t "$IMAGE" . > /tmp/xplat-build.log 2>&1 \
    || { tail -30 /tmp/xplat-build.log; die "amd64 image build (see /tmp/xplat-build.log)"; }
  say "build linux/amd64 image" "ok"
fi

[ -x ./dynajs ] || die "no ./dynajs -- build the host binary first"

if ! ./dynajs -e 'if (!Object.getOwnPropertyNames(globalThis).length) throw 0;
                  import("dyna:mathx").catch(() => { throw new Error("no native modules") })' >/dev/null 2>&1; then
  die "host ./dynajs has no dyna:* modules (a sanitizer build likely clobbered
     it). Rebuild:  ./build.sh build CONFIG_NATIVE_MODULES=y"
fi
for t in "${TESTS[@]}"; do
  [ -f "$t" ] || { say "$(basename "$t")" "SKIP (missing)"; continue; }
  host=$(./dynajs "$t" 2>&1 | shasum | cut -d' ' -f1)
  guest=$(docker run --rm -i --platform linux/amd64 "$IMAGE" \
            sh -c 'cat > /t.js && ./dynajs /t.js' < "$t" 2>&1 | shasum | cut -d' ' -f1)
  if [ "$host" = "$guest" ]; then
    say "$(basename "$t")" "ok  (arm64==amd64, sha ${host:0:12})"
  else
    say "$(basename "$t")" "MISMATCH arm64=${host:0:12} amd64=${guest:0:12}"
    fail=1
  fi
done

CC_TESTS="tests/test_utf8_ingress.c"
for t in $CC_TESTS; do
  [ -f "$t" ] || { say "$(basename "$t")" "SKIP (missing)"; continue; }
  b=$(basename "$t" .c)
  hostbin="/tmp/$b.$$.host"
  if ! clang -I. -Isrc -O2 -o "$hostbin" "$t" libdynajs.a -lm -lpthread 2>/dev/null; then
    say "$(basename "$t")" "SKIP (host build failed -- run ./build.sh build first)"; continue
  fi
  host=$("$hostbin" | shasum | cut -d' ' -f1)
  guest=$(docker run --rm -i --platform linux/amd64 "$IMAGE" sh -c \
            "cat > /$b.c && clang -I. -Isrc -O2 -o /$b /$b.c libdynajs.a -lm -lpthread && /$b" \
            < "$t" 2>/dev/null | shasum | cut -d' ' -f1)
  if [ "$host" = "$guest" ]; then
    say "$(basename "$t")" "ok  (arm64==amd64, sha ${host:0:12})"
  else
    say "$(basename "$t")" "MISMATCH arm64=${host:0:12} amd64=${guest:0:12}"
    fail=1
  fi
done

XLOG="/tmp/xplat-$$.log"
for target in "test-core TEST_SCOPE=all" "test-native TEST_SCOPE=all"; do
  if docker run --rm --platform linux/amd64 "$IMAGE" sh -c "./build.sh $target CONFIG_CLANG=y CONFIG_NATIVE_MODULES=y CONFIG_HARDEN= CONFIG_WERROR=" \
       >"$XLOG" 2>&1; then
    say "amd64 ./build.sh $target" "ok"
  else
    say "amd64 ./build.sh $target" "FAIL (see $XLOG)"; fail=1
  fi
done

MEMJS='globalThis.__k=(function(){const a=new Array(200000);
for(let i=0;i<200000;i++)a[i]={x:i,y:i,z:i,w:i};return a})();'
hm=$(echo "$MEMJS" > /tmp/_mem.js; ./dynajs -d /tmp/_mem.js 2>/dev/null | awk '/^memory allocated/{print $4}')
gm=$(docker run --rm -i --platform linux/amd64 "$IMAGE" \
       sh -c 'cat > /m.js && ./dynajs -d /m.js' <<< "$MEMJS" 2>/dev/null \
     | awk '/^memory allocated/{print $4}')
if [ -n "$hm" ] && [ -n "$gm" ]; then
  pct=$(awk "BEGIN{printf \"%.2f\", ($gm-$hm)/$hm*100}")
  ok=$(awk "BEGIN{print (($gm-$hm)/$hm*100 < 2 && ($gm-$hm)/$hm*100 > -2) ? 1 : 0}")
  if [ "$ok" = 1 ]; then say "memory arm64 vs amd64" "ok  (${pct}%, ${hm} vs ${gm} B)"
  else say "memory arm64 vs amd64" "DRIFT ${pct}% (${hm} vs ${gm} B)"; fail=1; fi
else
  say "memory arm64 vs amd64" "SKIP (no accounting output)"
fi

rm -f /tmp/_mem.js
[ "$fail" = 0 ] && echo "xplat: ok" || echo "xplat: FAIL"
exit $fail
