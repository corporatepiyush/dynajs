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
# copy is an INCLUDE list (owner directive) — only what the build consumes,
# the same set build.sh's TREE_NEEDS names plus docker/ — so test262, .obj,
# .git and host artifacts never enter the container at all. Build the image once with:
#
#     docker build --platform linux/arm64 --target deps -f docker/Dockerfile -t dynajs:deps .
#
# Examples:
#     docker/linux.sh ./build.sh build CONFIG_CLANG=y CONFIG_NATIVE_MODULES=y
#     docker/linux.sh --amd64 ./dynajs tests/test_http.js
#     docker/linux.sh ./build.sh build CONFIG_CLANG=y CONFIG_NATIVE_MODULES=y CONFIG_IO_URING=y
#
# seccomp=unconfined is NOT optional for io_uring: docker's stock profile denies
# io_uring_setup, so a probe reports "unsupported" on a kernel that supports it.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
PLATFORM=linux/arm64
[ "${1:-}" = "--amd64" ] && { PLATFORM=linux/amd64; shift; }
[ $# -eq 0 ] && { echo "usage: $0 [--amd64] <command...>" >&2; exit 2; }

IMG="dynajs:deps-${PLATFORM#linux/}"
docker image inspect "$IMG" >/dev/null 2>&1 || {
    echo "building $IMG (once)..." >&2
    docker build --platform "$PLATFORM" --target deps -f "$ROOT/docker/Dockerfile" \
        -t "$IMG" "$ROOT" >/dev/null || exit 1
}

VOL="dynajs-obj-${PLATFORM#linux/}"
docker volume inspect "$VOL" >/dev/null 2>&1 || docker volume create "$VOL" >/dev/null

JOBS=${JOBS:-$(docker info --format '{{.NCPU}}' 2>/dev/null || echo 4)}

exec docker run --rm --init --pull=never \
    --platform "$PLATFORM" --security-opt seccomp=unconfined \
    --cpus "$JOBS" --memory 12g --tmpfs /tmp:exec,size=2g \
    -e "JOBS=$JOBS" -e DEV_BUILD_J="$JOBS" \
    -v "$ROOT:/src:ro" -v "$VOL:/work/.obj" -w /work "$IMG" sh -c '
        copied=
        for e in build.sh VERSION .build-variant repl.js repl.c dynajs.d.ts \
                 install.sh README.md src tests third_party tools \
                 examples bench docker pgo-data hello.c test_fib.c; do
            [ -e "/src/$e" ] && copied="$copied $e"
        done
        [ -n "$copied" ] || { echo "linux.sh: nothing to copy from /src" >&2; exit 2; }
        tar -C /src -cf - $copied 2>/dev/null | tar -C /work -xf -
        exec "$@"' sh "$@"
