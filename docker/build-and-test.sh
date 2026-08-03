#!/usr/bin/env bash
# Build dynascript and run its in-tree test suite under both libc
# implementations (glibc via Debian, musl via Alpine), each with clang/LLVM.
# `make test` runs inside each image build, so a failing suite fails the build.
# Exits non-zero if either libc fails.
set -u

cd "$(dirname "$0")/.."   # repo root = docker build context

run_one() {
    local name=$1 target=$2 platform=${3:-}
    local platarg=()
    [ -n "$platform" ] && platarg=(--platform "$platform")
    echo "=============================================================="
    echo ">> building + testing dynascript on ${name}${platform:+ (${platform})}"
    echo "=============================================================="
    if docker build "${platarg[@]}" --target "${target}" -f docker/Dockerfile \
           -t "dynascript:${name}" . ; then
        echo ">> ${name}: PASS"
        return 0
    else
        echo ">> ${name}: FAIL"
        return 1
    fi
}

rc=0
run_one glibc glibc || rc=1
run_one musl  musl  || rc=1
# emulated amd64 (qemu-x86_64): exercises the SSE4.2/AVX2 SIMD kernels. Slow.
# Needs binfmt: docker run --privileged --rm tonistiigi/binfmt --install amd64
run_one amd64 amd64 linux/amd64 || rc=1

echo "=============================================================="
if [ "$rc" -eq 0 ]; then
    echo "RESULT: both glibc and musl PASS"
else
    echo "RESULT: at least one libc FAILED"
fi
echo "=============================================================="
exit "$rc"
