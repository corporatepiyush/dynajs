#!/usr/bin/env bash
# Run dynascript's test battery on Linux under glibc (Debian), musl (Alpine),
# and emulated amd64 (qemu-x86_64) -- against CACHED toolchain images, so a run
# pays no package installs and creates no images.
#
# The toolchain lives in three cached images built from docker/Dockerfile
# (targets `deps` and `deps-musl`, one per platform), built ONCE when missing
# and reused from then on -- the docker/linux.sh pattern. The test runs are
# EPHEMERAL (--rm) containers over a copied tree, and .obj lives in PERSISTENT
# volumes keyed by platform, so a repeat run only recompiles what changed
# (measured ~5 min fresh vs ~12 s warm on the amd64 side).
#
#   docker/build-and-test.sh            all three legs, concurrently
#   docker/build-and-test.sh glibc      one leg
#
# The legs are independent (own container, own .obj volume), so they fan out in
# parallel, with the VM's cores split across them. amd64 runs under qemu
# emulation and is slow by construction. Exits non-zero if any leg fails.
set -u

cd "$(dirname "$0")/.." || exit 1   # repo root = docker build context
ROOT=$PWD

JOBS=${JOBS:-$(docker info --format '{{.NCPU}}' 2>/dev/null || echo 4)}
MK="CONFIG_CLANG=y CONFIG_NATIVE_MODULES=y CONFIG_TLS=y"

# ---- ensure the cached toolchain image exists (build once, reuse forever) ----
# The tag is PER-PLATFORM: `docker image inspect` does not check architecture,
# so one tag cannot serve both. A tag built before the Dockerfile changed stays
# as-is; rebuild it explicitly when the package list moves.
ensure_image() {   # $1=tag  $2=target  $3=platform(optional)
    local tag=$1 target=$2 plat=()
    [ -n "${3:-}" ] && plat=(--platform "$3")
    docker image inspect "$tag" >/dev/null 2>&1 && return 0
    echo "building cached toolchain image $tag (once)..." >&2
    docker build "${plat[@]}" --target "$target" -f "$ROOT/docker/Dockerfile" \
        -t "$tag" "$ROOT" >/dev/null || return 1
}

# ---- one leg: ephemeral container over the cached image -------------------
# The tree is copied with the same excludes as docker/linux.sh: test262 is
# 265 MB and no test here reads it; .obj comes from the persistent volume.
run_leg() {   # $1=name  $2=tag  $3=volume  $4=leg-script  rest=docker-run args
    local name=$1 tag=$2 vol=$3 script=$4; shift 4
    echo "=============================================================="
    echo ">> testing dynascript on ${name}"
    echo "=============================================================="
    docker volume inspect "$vol" >/dev/null 2>&1 || docker volume create "$vol" >/dev/null
    if docker run --rm --init --pull=never "$@" \
        --security-opt seccomp=unconfined \
        --cpus "$LEG_JOBS" --memory 12g --tmpfs /tmp:exec,size=2g \
        -e "JOBS=$LEG_JOBS" -e "MK=$MK" \
        -v "$ROOT:/src:ro" -v "$vol:/work/.obj" -v "$script:/tmp/leg.sh:ro" \
        -w /work "$tag" sh -c '
            tar -C /src -cf - --exclude=./test262 --exclude=./.obj --exclude=./.git \
                --exclude=./third_party . 2>/dev/null | tar -C /work -xf -
            exec sh /tmp/leg.sh'; then
        echo ">> ${name}: PASS"
        return 0
    else
        echo ">> ${name}: FAIL"
        return 1
    fi
}

# ---- the legs -------------------------------------------------------------
# Each is a standalone script so quoting survives the container boundary.
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

cat > "$TMP/glibc.sh" <<'EOF'
set -e
make $MK -j"$JOBS"
make $MK test
EOF

cat > "$TMP/musl.sh" <<'EOF'
set -e
make $MK -j"$JOBS"
make $MK test
make $MK test-native
EOF

# The amd64 battery: caps probe (which ISA emulation actually dispatches to),
# vectorisation audit, the full suites, the SIMD tests, the hardware-crc/sha
# differentials, and the ml oracle built twice and diffed.
cat > "$TMP/amd64.sh" <<'EOF'
set -e
make $MK -j"$JOBS"
printf '#include "dyna-simd-kernels.h"\n#include <stdio.h>\nint main(void){simd_init();unsigned long long c=cpu_features();printf("SIMD caps=0x%%llx  SSE42=%%d AVX2=%%d AVX512=%%d NEON=%%d\\n",c,!!(c&CPU_SSE42),!!(c&CPU_AVX2),!!(c&CPU_AVX512F),!!(c&CPU_NEON));return 0;}' > /tmp/caps.c
clang -O2 -Isrc /tmp/caps.c .obj/dyna-simd-core.o .obj/dyna-simd-scalar.o \
      .obj/dyna-simd-sse42.o .obj/dyna-simd-avx2.o .obj/dyna-simd-avx512.o \
      .obj/dyna-simd-neon.o .obj/dyna-simd-sve.o $LDFLAGS -lpthread -lm -o /tmp/caps
echo "=== x86 SIMD dispatch under qemu ==="
/tmp/caps
echo "=== x86-64 vectorization audit (baseline, no -march) ==="
bash tools/vecaudit.sh src/dyna-ml.c | sort -t= -k2 -rn | head -16
echo "--- libm calls per function (a call in a loop body blocks the vectoriser) ---"
bash tools/libm-in-loops.sh src/dyna-ml.c
make $MK test
make $MK test-native
make $MK test-security
./dynajs tests/test_simd.js
./dynajs tests/test_simd_f64.js
./dynajs tests/test_simd_int.js
make $MK test-crc32c-hw
make $MK test-sha256-hw
./dynajs tests/test_ml_oracle.js > /tmp/ml_vec.txt
make clean >/dev/null
make $MK CONFIG_ML_NO_SIMD=y -j"$JOBS" >/dev/null
./dynajs tests/test_ml_oracle.js > /tmp/ml_seq.txt
echo "=== dyna:ml oracle diff on x86-64 ==="
./dynajs tests/test_ml_oracle.js --diff /tmp/ml_vec.txt /tmp/ml_seq.txt
EOF

rc=0
legs="${*:-glibc musl amd64}"
# The legs are independent (own container, own .obj volume), so they run
# CONCURRENTLY. The VM's cores are split across them: each container's --cpus
# caps its own -j, so the inner parallelism stays real without the legs
# thrashing one another. The amd64 leg pays for qemu out of its own share.
nlegs=$(printf '%s\n' "$legs" | grep -c .)
LEG_JOBS=$(( (JOBS + nlegs - 1) / nlegs ))
[ "$LEG_JOBS" -ge 2 ] || LEG_JOBS=2

# Every toolchain image must exist before the fan-out (serial, quick: cached).
for leg in $legs; do
    case "$leg" in
        glibc) ensure_image dynajs:deps deps || rc=1 ;;
        musl)  ensure_image dynajs:deps-musl deps-musl || rc=1 ;;
        amd64) ensure_image dynajs:deps-amd64 deps linux/amd64 || rc=1 ;;
        *)     echo "unknown leg: $leg (want glibc, musl, amd64)" >&2; rc=1 ;;
    esac
done

# Fan out. Each leg owns its log, so concurrent output cannot interleave; on a
# failure the tail is printed with the verdict. The trap is reset in each
# backgrounded subshell: a leg finishing would otherwise run the script's EXIT
# trap and delete $TMP (the other legs' logs and scripts) mid-run.
pids=""
for leg in $legs; do
    case "$leg" in
        glibc) ( trap - EXIT; run_leg glibc dynajs:deps dynajs-obj-arm64 "$TMP/glibc.sh" ) \
                   >"$TMP/glibc.log" 2>&1 & pids="$pids glibc:$!" ;;
        musl)  ( trap - EXIT; run_leg musl dynajs:deps-musl dynajs-obj-musl "$TMP/musl.sh" ) \
                   >"$TMP/musl.log" 2>&1 & pids="$pids musl:$!" ;;
        amd64) ( trap - EXIT; run_leg amd64 dynajs:deps-amd64 dynajs-obj-amd64 "$TMP/amd64.sh" \
                   --platform linux/amd64 -e QEMU_CPU=Haswell \
                   -e 'LDFLAGS=-g -fuse-ld=lld --rtlib=compiler-rt' \
                   -e DYNAJS_REQUIRE_TOOLS=1 ) \
                   >"$TMP/amd64.log" 2>&1 & pids="$pids amd64:$!" ;;
    esac
done
for p in $pids; do
    leg="${p%%:*}"; pid="${p##*:}"
    wait "$pid" || rc=1
    grep -q ">> ${leg}: PASS" "$TMP/$leg.log" 2>/dev/null || rc=1
    echo "----- $leg log tail -----"
    tail -4 "$TMP/$leg.log" 2>/dev/null
done

echo "=============================================================="
if [ "$rc" -eq 0 ]; then
    echo "RESULT: all legs PASS"
else
    echo "RESULT: at least one leg FAILED"
fi
echo "=============================================================="
exit "$rc"
