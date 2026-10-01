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

cd "$(dirname "$0")/.." || exit 1
ROOT=$PWD

JOBS=${JOBS:-$(docker info --format '{{.NCPU}}' 2>/dev/null || echo 4)}
# the hardened warning policy trips on the x86 SIMD sources under qemu --
# the container legs keep the legacy unhardened flags
MK="CONFIG_CLANG=y CONFIG_NATIVE_MODULES=y CONFIG_TLS=y CONFIG_HARDEN= CONFIG_WERROR="

ensure_image() {
    local tag=$1 target=$2 plat=()
    [ -n "${3:-}" ] && plat=(--platform "$3")
    docker image inspect "$tag" >/dev/null 2>&1 && return 0
    echo "building cached toolchain image $tag (once)..." >&2
    docker build "${plat[@]}" --target "$target" -f "$ROOT/docker/Dockerfile" \
        -t "$tag" "$ROOT" >/dev/null || return 1
}

ARCHIVE=""
run_leg() {
    local name=$1 tag=$2 vol=$3 script=$4; shift 4
    echo "=============================================================="
    echo ">> testing dynascript on ${name}"
    echo "=============================================================="
    docker volume inspect "$vol" >/dev/null 2>&1 || docker volume create "$vol" >/dev/null
    ARCHIVE=$(mktemp "$SCRATCH/src.XXXXXX") || return 1
    git -C "$ROOT" archive --format=tar HEAD > "$ARCHIVE" || {
        rm -f "$ARCHIVE"; return 1; }
    echo "   (source archive: $(du -h "$ARCHIVE" | cut -f1) of tracked files)"
    if docker run --rm --init --pull=never "$@" \
        --security-opt seccomp=unconfined \
        --cpus "$LEG_JOBS" --memory 12g --tmpfs /tmp:exec,size=2g \
        -e "JOBS=$LEG_JOBS" -e "MK=$MK" \
        -v "$vol:/work/.obj" -v "$ARCHIVE:/tmp/src.tar:ro" -v "$script:/tmp/leg.sh:ro" \
        -w /work "$tag" sh -c '
            set -e
            tar -C /work -xf /tmp/src.tar
            # git-archive mtimes are commit-time, older than objects left in
            # the persistent .obj volume by a previous run -- the staleness
            # check then skips every file and the leg tests a STALE binary
            # (measured: the amd64 leg ran a binary whose dyna:* modules
            # could not load). Bump sources to now so the engine rebuilds
            # what changed.
            find /work/src /work/tests /work/build.sh -newer /tmp/src.tar \
                -o -path /work/src -o -path /work/tests \
                -o -name '*.c' -o -name '*.h' -o -name '*.inc.c' \
                -o -name build.sh 2>/dev/null | xargs touch 2>/dev/null || true
            find /work/src /work/tests /work/build.sh -exec touch {} + 2>/dev/null || true
            exec sh /tmp/leg.sh'; then
        echo ">> ${name}: PASS"
        rm -f "$ARCHIVE"; ARCHIVE=""
        return 0
    else
        echo ">> ${name}: FAIL"
        rm -f "$ARCHIVE"; ARCHIVE=""
        return 1
    fi
}

SCRATCH="${HOME}/.cache/dynajs-docker"
mkdir -p "$SCRATCH"
TMP="$(mktemp -d "$SCRATCH/run.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

cat > "$TMP/glibc.sh" <<'EOF'
set -e
DEV_BUILD_J="$JOBS" ./build.sh build $MK
./build.sh test TEST_SCOPE=all $MK
EOF

cat > "$TMP/musl.sh" <<'EOF'
set -e
DEV_BUILD_J="$JOBS" ./build.sh build $MK
./build.sh test TEST_SCOPE=all $MK
./build.sh test-native $MK TEST_SCOPE=all
EOF

cat > "$TMP/amd64.sh" <<'EOF'
set -e
DEV_BUILD_J="$JOBS" ./build.sh build $MK
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
./build.sh test TEST_SCOPE=all $MK
./build.sh test-native $MK TEST_SCOPE=all
./build.sh test-security
./dynajs tests/test_simd.js
./dynajs tests/test_simd_f64.js
./dynajs tests/test_simd_int.js
./build.sh test-crc32c-hw
./build.sh test-sha256-hw
./dynajs tests/test_ml_oracle.js > /tmp/ml_vec.txt
./build.sh clean >/dev/null
DEV_BUILD_J="$JOBS" ./build.sh build $MK CONFIG_ML_NO_SIMD=y >/dev/null
./dynajs tests/test_ml_oracle.js > /tmp/ml_seq.txt
echo "=== dyna:ml oracle diff on x86-64 ==="
./dynajs tests/test_ml_oracle.js --diff /tmp/ml_vec.txt /tmp/ml_seq.txt
EOF

rc=0
legs="${*:-glibc musl amd64}"
nlegs=$(printf '%s\n' "$legs" | grep -c .)
LEG_JOBS=$(( (JOBS + nlegs - 1) / nlegs ))
[ "$LEG_JOBS" -ge 2 ] || LEG_JOBS=2

for leg in $legs; do
    case "$leg" in
        glibc) ensure_image dynajs:deps deps || rc=1 ;;
        musl)  ensure_image dynajs:deps-musl deps-musl || rc=1 ;;
        amd64) ensure_image dynajs:deps-amd64 deps linux/amd64 || rc=1 ;;
        *)     echo "unknown leg: $leg (want glibc, musl, amd64)" >&2; rc=1 ;;
    esac
done

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
