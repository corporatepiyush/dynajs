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
#   docker/build-and-test.sh            the three portability legs, concurrently
#   docker/build-and-test.sh glibc      one leg
#   docker/build-and-test.sh gcc uring  the strict-GCC legs (see below)
#
# Legs:  glibc musl amd64   clang, unhardened flags, the wide portability sweep
#        gcc                GCC with the full hardening + -Werror policy the
#                           native build uses, epoll backend + TLS, every suite
#        uring              the same policy on the io_uring backend + TLS
# The source under test is the WORKING TREE (tracked files plus untracked ones
# that are not ignored), so a change can be proved on Linux before it is
# committed; DOCKER_SRC=head tests the committed tree instead.
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
# The emulated x86-64 leg wants a Haswell (the AVX2 tier). A qemu older than
# 7.2 cannot emulate AVX/AVX2/FMA: it drops those bits and prints "TCG doesn't
# support requested feature" on the stderr of EVERY process, which corrupts any
# test that reads a child's output. On such a host use `max` -- the same
# effective feature set (SSE4.2 tier), silently -- and say that the AVX2 tier
# is NOT exercised by this leg here.
amd64_cpu_model() {
    if docker run --rm --pull=never --platform linux/amd64 -e QEMU_CPU=Haswell \
            dynajs:deps-amd64 true 2>&1 | grep -q "TCG doesn't support requested feature"; then
        echo "   (this host's qemu cannot emulate AVX2: x86-64 leg runs the SSE4.2 tier only)" >&2
        echo max
    else
        echo Haswell
    fi
}

# Two sandboxes can each deny io_uring on a kernel that supports it: the
# runtime's seccomp profile, and SELinux on hosts that enforce it (Fedora
# CoreOS, i.e. a podman machine: queue_init fails with EACCES). Both are lifted
# for the legs; label=disable is a no-op where SELinux is absent.
run_leg() {
    local name=$1 tag=$2 vol=$3 script=$4; shift 4
    echo "=============================================================="
    echo ">> testing dynascript on ${name}"
    echo "=============================================================="
    docker volume inspect "$vol" >/dev/null 2>&1 || docker volume create "$vol" >/dev/null
    ARCHIVE=$(mktemp "$SCRATCH/src.XXXXXX") || return 1
    if [ "${DOCKER_SRC:-worktree}" = head ]; then
        git -C "$ROOT" archive --format=tar HEAD > "$ARCHIVE" || {
            rm -f "$ARCHIVE"; return 1; }
        echo "   (source archive: $(du -h "$ARCHIVE" | cut -f1) of the committed tree)"
    else
        ( cd "$ROOT" && git ls-files -co --exclude-standard | while IFS= read -r f; do
              [ -e "$f" ] && printf '%s\n' "$f"; done > "$ARCHIVE.list" \
          && COPYFILE_DISABLE=1 tar --no-xattrs -cf "$ARCHIVE" -T "$ARCHIVE.list" ) || {
            rm -f "$ARCHIVE" "$ARCHIVE.list"; return 1; }
        rm -f "$ARCHIVE.list"
        echo "   (source archive: $(du -h "$ARCHIVE" | cut -f1) of the working tree)"
    fi
    if docker run --rm --init --pull=never "$@" \
        --security-opt seccomp=unconfined --security-opt label=disable \
        --cpus "$LEG_JOBS" --memory 12g --tmpfs /tmp:exec,size=2g \
        -e "JOBS=$LEG_JOBS" -e "MK=$MK" \
        -v "$vol:/work/.obj" -v "$ARCHIVE:/tmp/src.tar:ro" -v "$script:/tmp/leg.sh:ro" \
        -v "$TMP/lib.sh:/tmp/lib.sh:ro" \
        -w /work "$tag" sh -c '
            set -e
            tar -C /work -xf /tmp/src.tar
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

# A compiler that dies under emulation (qemu makes gcc/clang segfault on a
# random file now and then) is retried; a real diagnostic is not.
cat > "$TMP/lib.sh" <<'EOF'
build_retry() {
    local try
    for try in 1 2 3 4 5; do
        if ./build.sh build "$@" > /tmp/build.log 2>&1; then tail -1 /tmp/build.log; return 0; fi
        if grep -qE "error: " /tmp/build.log \
           || ! grep -qE "internal compiler error|Segmentation fault|rc=139|rc=4\)" /tmp/build.log; then
            cat /tmp/build.log; return 1
        fi
        echo "   (the compiler crashed under emulation; retry $try)"
    done
    cat /tmp/build.log; return 1
}
EOF

cat > "$TMP/gcc.sh" <<'EOF'
set -e
. /tmp/lib.sh
STRICT="CONFIG_NATIVE_MODULES=y CONFIG_TLS=y"
DEV_BUILD_J="$JOBS" build_retry $STRICT
./build.sh test-native $STRICT TEST_SCOPE=all
./build.sh test-pool
./build.sh test-aio-cpu-offload
./build.sh test-aio-tls $STRICT
EOF

cat > "$TMP/uring.sh" <<'EOF'
set -e
. /tmp/lib.sh
STRICT="CONFIG_NATIVE_MODULES=y CONFIG_IO_URING=y CONFIG_TLS=y"
DEV_BUILD_J="$JOBS" build_retry $STRICT
./build.sh test-native $STRICT TEST_MODS=net,http,scrape,tls,oauth2,sys,bytes,encoding,crypto-hash
./build.sh test-aio-tls $STRICT
EOF

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
. /tmp/lib.sh
DEV_BUILD_J="$JOBS" build_retry $MK
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
./dynajs --std tests/test_ml_oracle.js > /tmp/ml_vec.txt
./build.sh clean >/dev/null
DEV_BUILD_J="$JOBS" ./build.sh build $MK CONFIG_ML_NO_SIMD=y >/dev/null
./dynajs --std tests/test_ml_oracle.js > /tmp/ml_seq.txt
echo "=== dyna:ml oracle diff on x86-64 ==="
./dynajs --std tests/test_ml_oracle.js --diff /tmp/ml_vec.txt /tmp/ml_seq.txt
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
        gcc|uring) ensure_image dynajs:deps deps || rc=1 ;;
        *)     echo "unknown leg: $leg (want glibc, musl, amd64, gcc, uring)" >&2; rc=1 ;;
    esac
done

pids=""
for leg in $legs; do
    case "$leg" in
        glibc) ( trap - EXIT; run_leg glibc dynajs:deps dynajs-obj-arm64 "$TMP/glibc.sh" ) \
                   >"$TMP/glibc.log" 2>&1 & pids="$pids glibc:$!" ;;
        gcc)   ( trap - EXIT; run_leg gcc dynajs:deps dynajs-obj-gcc "$TMP/gcc.sh" ) \
                   >"$TMP/gcc.log" 2>&1 & pids="$pids gcc:$!" ;;
        uring) ( trap - EXIT; run_leg uring dynajs:deps dynajs-obj-uring "$TMP/uring.sh" ) \
                   >"$TMP/uring.log" 2>&1 & pids="$pids uring:$!" ;;
        musl)  ( trap - EXIT; run_leg musl dynajs:deps-musl dynajs-obj-musl "$TMP/musl.sh" ) \
                   >"$TMP/musl.log" 2>&1 & pids="$pids musl:$!" ;;
        amd64) ( trap - EXIT; run_leg amd64 dynajs:deps-amd64 dynajs-obj-amd64 "$TMP/amd64.sh" \
                   --platform linux/amd64 -e "QEMU_CPU=$(amd64_cpu_model)" -e DEV_SUITE_TIME_SCALE=12 \
                   -e 'LDFLAGS=-g -fuse-ld=lld --rtlib=compiler-rt' \
                   -e DYNAJS_REQUIRE_TOOLS=1 ) \
                   >"$TMP/amd64.log" 2>&1 & pids="$pids amd64:$!" ;;
    esac
done
for p in $pids; do
    leg="${p%%:*}"; pid="${p##*:}"
    wait "$pid" || rc=1
    grep -q ">> ${leg}: PASS" "$TMP/$leg.log" 2>/dev/null || rc=1
    mkdir -p "$ROOT/.dev/linux-logs" 2>/dev/null \
        && cp "$TMP/$leg.log" "$ROOT/.dev/linux-logs/$leg.log" 2>/dev/null \
        && echo "----- $leg: full log kept at .dev/linux-logs/$leg.log -----"
    echo "----- $leg log tail -----"
    if grep -q ">> ${leg}: PASS" "$TMP/$leg.log" 2>/dev/null; then
        tail -4 "$TMP/$leg.log" 2>/dev/null
    else
        grep -nE "^FAIL|  FAIL|error: |internal compiler" "$TMP/$leg.log" 2>/dev/null | head -30
        tail -15 "$TMP/$leg.log" 2>/dev/null
    fi
done

echo "=============================================================="
if [ "$rc" -eq 0 ]; then
    echo "RESULT: all legs PASS"
else
    echo "RESULT: at least one leg FAILED"
fi
echo "=============================================================="
exit "$rc"
