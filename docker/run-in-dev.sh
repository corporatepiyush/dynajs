#!/usr/bin/env bash
# Build + run something against dynascript inside the prebuilt dynascript-dev
# toolchain image (no per-run apt). Source is copied into the container so the
# build can't mutate the host tree.
#
#   docker/run-in-dev.sh amd64 'make CONFIG_CLANG=y CONFIG_NATIVE_MODULES=y -j2 dynajs && ./dynajs tests/test_text_simd.js'
#   docker/run-in-dev.sh arm64 'make CONFIG_CLANG=y CONFIG_NATIVE_MODULES=y CONFIG_IO_URING=y ... '
#
# Pass --uring-seccomp as a 3rd arg to drop the seccomp profile (io_uring runtime).
set -u
arch="${1:?usage: run-in-dev.sh <amd64|arm64> <command> [--uring-seccomp]}"
cmd="${2:?missing command}"
seccomp=()
[ "${3:-}" = "--uring-seccomp" ] && seccomp=(--security-opt seccomp=unconfined)

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
docker run --rm --platform "linux/${arch}" "${seccomp[@]}" \
    -v "${repo_root}:/host:ro" "dynascript-dev:${arch}" bash -c "
set -e
rsync -a --exclude=test262 --exclude=.git --exclude=.obj --exclude=dynajs --exclude=dynajsc /host/ /build/
cd /build
${cmd}
"
