#!/bin/sh
# linux.sh [--amd64] <command...> -- run a command against this tree inside a
# Linux container, fast.
#
# WHY THIS EXISTS. The obvious `docker run -v $PWD:/src ... cp -r /src/. /work`
# recipe costs, EVERY TIME:
#   * 529 MB copied, of which test262 alone is 265 MB and no build needs it
#   * an apt-get install of ~8 packages
#   * a full engine rebuild from zero on the VM's 4 CPUs
# Measured: that is minutes per run, and the deps and the copy are pure waste.
#
# So: the deps live in a CACHED image (docker/Dockerfile, target `deps`), and the
# copy excludes test262/.obj/.git. Build the image once with:
#
#     docker build --platform linux/arm64 --target deps -f docker/Dockerfile -t dynajs:deps .
#
# Examples:
#     docker/linux.sh make CONFIG_CLANG=y CONFIG_NATIVE_MODULES=y
#     docker/linux.sh --amd64 ./dynajs tests/test_http.js
#     docker/linux.sh make CONFIG_CLANG=y CONFIG_NATIVE_MODULES=y CONFIG_IO_URING=y
#
# seccomp=unconfined is NOT optional for io_uring: docker's stock profile denies
# io_uring_setup, so a probe reports "unsupported" on a kernel that supports it.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
PLATFORM=linux/arm64
[ "${1:-}" = "--amd64" ] && { PLATFORM=linux/amd64; shift; }
[ $# -eq 0 ] && { echo "usage: $0 [--amd64] <command...>" >&2; exit 2; }

docker image inspect dynajs:deps >/dev/null 2>&1 || {
    echo "building dynajs:deps (once)..." >&2
    docker build --platform "$PLATFORM" --target deps -f "$ROOT/docker/Dockerfile" \
        -t dynajs:deps "$ROOT" >/dev/null || exit 1
}

# --exclude keeps the 265 MB test262 checkout and the host's object dir out of
# the container: a volume mount ignores .dockerignore, so it must be done here.
# A PERSISTENT object cache per platform. Without it every run recompiles all
# 84 objects from scratch -- measured ~5 min in the VM against 12s natively.
# With it the second run only rebuilds what changed. The volume is keyed by
# platform because an arm64 object cannot link into an amd64 binary.
VOL="dynajs-obj-${PLATFORM#linux/}"
docker volume inspect "$VOL" >/dev/null 2>&1 || docker volume create "$VOL" >/dev/null

exec docker run --rm --platform "$PLATFORM" --security-opt seccomp=unconfined \
    -v "$ROOT:/src:ro" -v "$VOL:/work/.obj" -w /work dynajs:deps sh -c '
        tar -C /src -cf - --exclude=./test262 --exclude=./.obj --exclude=./.git \
            --exclude=./third_party . 2>/dev/null | tar -C /work -xf -
        exec "$@"' sh "$@"
