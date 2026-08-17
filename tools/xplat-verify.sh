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
# Exit nonzero on any mismatch. Terse output, like dev.sh.
set -uo pipefail
cd "$(dirname "$0")/.." || exit 2

IMAGE=dynajs:xplat
DOCKERFILE=docker/Dockerfile
DOCKER_TARGET=glibc
# --build is now the DEFAULT and the flag is accepted only so old invocations
# keep working; the image is rebuilt every run (see the note at the build).
[ "${1:-}" = "--build" ] && shift

# Differential tests: each must produce identical output on both platforms.
# Add every new oracle-style test here.
TESTS=("$@")
[ ${#TESTS[@]} -eq 0 ] && TESTS=(tests/test_regexp_prefilter.js tests/test_dataframe.js
                                 tests/test_object_literal_presize.js
                                 # every SIMD-reaching path added since: the x86
                                 # kernels below these NEVER run on the dev host
                                 tests/oracle_string_indexof.js
                                 tests/test_matcher.js
                                 # NOT test_lz4.js: its assertion count depends
                                 # on whether an `lz4` binary exists, so its
                                 # hash differs for a reason that is not
                                 # portability. This one is a pure function of
                                 # the code, and it pins the COMPRESSED BYTES,
                                 # which is what an endianness bug in the SWAR
                                 # match finder would change without ever
                                 # failing a round trip.
                                 tests/oracle_compress_bytes.js
                                 # dyn-dict is a NEW BYTE FORMAT (Varint codes +
                                 # a CRC-32C dictionary id) and its parse is a
                                 # DP, so a portability bug would change the
                                 # emitted stream while still round-tripping --
                                 # the same shape as the SWAR match-finder bug
                                 # oracle_compress_bytes.js exists for.
                                 tests/test_dictionary.js
                                 # the Bytes ASCII scan is new SWAR: a 64-bit
                                 # load and a high-bit test. Endianness-neutral
                                 # by construction, which is what this checks.
                                 tests/test_bytes_handle.js
                                 tests/test_hash_split.js
                                 tests/test_crypto.js
                                 tests/test_ml_boosting.js
                                 tests/test_iterator_lazy.js
                                 # W9.8: the histogram finder's bin codes are
                                 # bytes and its statistics are doubles, so a
                                 # divergence here would be a real portability
                                 # bug rather than a timing one -- the outputs
                                 # are assertion counts, which is a pure
                                 # function of the code.
                                 tests/oracle_ml_hist.js
                                 tests/test_ml_xgb.js
                                 # class CSR is a new index representation and
                                 # its normal equations run over nonzero pairs.
                                 tests/test_ml_sparse.js
                                 tests/test_ml_weights.js
                                 # the applicative generalisation and the
                                 # locale tables are both engine/table lookups
                                 # that no x86 kernel touches -- included so a
                                 # future one cannot silently start touching
                                 # them.
                                 tests/test_ext_batch8.js
                                 tests/test_time_dateparser.js
                                 tests/test_encoding.js)

fail=0
say(){ printf '%-46s %s\n' "$1" "$2"; }
die(){ echo "FAIL: $*" >&2; exit 1; }

command -v docker >/dev/null 2>&1 || die "docker not found"
docker info >/dev/null 2>&1 || die "docker daemon not running"

# ALWAYS build. The old condition was "--build was passed OR the image does not
# exist", so once dynajs:xplat existed it was reused forever however far the
# source moved: measured, the cached image exposed 16 DataFrame methods against
# the host's 167, and the script reported five "MISMATCH" lines that were not
# cross-platform bugs at all. A gate that invents defects is worse than none.
# Docker's own layer cache makes an unchanged context cheap, so this costs
# seconds when nothing moved and is correct when something did.
if true; then
  echo "building $IMAGE (linux/amd64, emulated -- cached layers make this fast"
  echo "  when the source has not moved; slow the first time)..."
  # NB: the `docker build` must be the LAST command in the pipeline, never
  # chained with `; echo`, or its exit code is masked by the echo's success.
  # --pull is load-bearing, not hygiene. The legacy builder walks every stage
  # ahead of the target, so the glibc target still evaluates `FROM base-musl AS
  # amd64`; if the locally cached alpine/debian base is the HOST's arm64 image
  # the build dies with "found but does not provide the specified platform" and
  # the whole gate fails for a reason that has nothing to do with the code.
  # BuildKit would skip those stages but needs buildx, which is not assumed here.
  docker build --pull --platform linux/amd64 --target "$DOCKER_TARGET" \
    -f "$DOCKERFILE" -t "$IMAGE" . > /tmp/xplat-build.log 2>&1 \
    || { tail -30 /tmp/xplat-build.log; die "amd64 image build (see /tmp/xplat-build.log)"; }
  say "build linux/amd64 + make test" "ok"
fi

# ---- 1. differential tests: arm64 host output vs amd64 container output ----
[ -x ./dynajs ] || die "no ./dynajs -- build the host binary first"

# The container ALWAYS has the dyna:* modules (the glibc stage builds
# CONFIG_NATIVE_MODULES=y). The host binary might not: a sanitizer build via
# dev.sh drops the flag, and because a bare `make` and `make CONFIG_ASAN=y`
# share the non-suffixed artifacts, a later rebuild without `make clean` can
# leave a dynajs that cannot load any module. Comparing that against the
# container yields a MISMATCH that looks like a portability bug and is not, so
# check it up front and say what to do.
# The probe names something STRUCTURAL, not a particular module: naming one
# makes this fail closed the day that module is retired, which is exactly what
# happened to `make test-native`'s dyna:bits probe (CLAUDE.md section 13) and
# had happened here too, unnoticed, since dyna:bits was absorbed into mathx.
if ! ./dynajs -e 'if (!Object.getOwnPropertyNames(globalThis).length) throw 0;
                  import("dyna:mathx").catch(() => { throw new Error("no native modules") })' >/dev/null 2>&1; then
  die "host ./dynajs has no dyna:* modules (a sanitizer build likely clobbered
     it). Rebuild:  make clean && mkdir -p .obj/examples .obj/tests &&
                    make CONFIG_NATIVE_MODULES=y -j\$(nproc)"
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

# ---- 1b. C differential harnesses -------------------------------------------
# These reach code JS cannot: malformed UTF-8 cannot be written from JS source,
# because every route in from JS has already been through a decoder.
CC_TESTS="tests/test_utf8_ingress.c"
for t in $CC_TESTS; do
  [ -f "$t" ] || { say "$(basename "$t")" "SKIP (missing)"; continue; }
  b=$(basename "$t" .c)
  if ! clang -I. -Isrc -O2 -o "/tmp/$b.host" "$t" libdynajs.a -lm -lpthread 2>/dev/null; then
    say "$(basename "$t")" "SKIP (host build failed -- run make first)"; continue
  fi
  host=$("/tmp/$b.host" | shasum | cut -d' ' -f1)
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

# ---- 2. suites inside the container -----------------------------------------
# The image build already ran both, but re-running catches an image built from
# an older tree, and reports which one broke.
# CONFIG_CLANG=y is not optional here: the image is BUILT with it, but a bare
# `make test` inherits none of the main build's flags, so $(CC) falls back to
# gcc -- which the image does not install. The failure reads as an x86-64 defect
# ("amd64 make test FAIL") and is a missing compiler.
for target in test test-native; do
  if docker run --rm --platform linux/amd64 "$IMAGE" sh -c "make CONFIG_CLANG=y CONFIG_NATIVE_MODULES=y $target" \
       >"/tmp/xplat-$target.log" 2>&1; then
    say "amd64 make $target" "ok"
  else
    say "amd64 make $target" "FAIL (see /tmp/xplat-$target.log)"; fail=1
  fi
done

# ---- 3. memory accounting must agree (architecture-real, emulation-safe) ----
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
