#!/usr/bin/env bash
# Build + run something against dynascript inside the prebuilt dynascript-dev
# toolchain image (no per-run apt). Source is copied into the container so the
# build can't mutate the host tree.
#
#   docker/run-in-dev.sh amd64 'CC=clang WERROR=0 ./build.sh release && tools/run-tests-parallel.sh tests/bytes/test_bytes_bytes_text_simd.js tests/bytes/test_bytes_is_valid_utf8_text_simd.js'
#   docker/run-in-dev.sh arm64 'CC=clang WERROR=0 IO_URING=1 ./build.sh release && ... '
#
# Pass --uring-seccomp as a 3rd arg to drop the seccomp profile (io_uring runtime).
set -u
arch="${1:?usage: run-in-dev.sh <amd64|arm64> <command> [--uring-seccomp]}"
cmd="${2:?missing command}"
seccomp=()
[ "${3:-}" = "--uring-seccomp" ] && seccomp=(--security-opt seccomp=unconfined)

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
tree_needs="build.sh VERSION repl.js dynajs.d.ts install.sh README.md src tests third_party tools examples bench"
docker run --rm --platform "linux/${arch}" "${seccomp[@]}" \
    -v "${repo_root}:/host:ro" "dynascript-dev:${arch}" bash -c "
set -e
mkdir -p /build
for e in ${tree_needs}; do [ -e /host/\$e ] && cp -a /host/\$e /build/; done
cd /build
${cmd}
"
