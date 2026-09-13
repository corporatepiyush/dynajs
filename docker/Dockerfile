# One multi-stage Dockerfile for every container gate. Replaces eleven
# near-identical files, five of which nothing referenced any more.
#
#   docker build --target <t> -f docker/Dockerfile .
#
#   deps         CACHED glibc toolchain for docker/linux.sh and
#                docker/build-and-test.sh. Built once, reused forever.
#   deps-musl    CACHED musl toolchain for docker/build-and-test.sh.
#   uring-tests  io_uring + port-binding tests, BOTH backends
#   uring-bench  io_uring throughput
#   bench        Linux benchmark run against Node
#   profile      perf/wrk profiling run
#   dev          interactive toolchain, no build
#
# The test battery itself (glibc, musl, emulated amd64) runs as EPHEMERAL
# containers against the cached deps images -- see docker/build-and-test.sh.
# Nothing here builds a test image per run, and a run pays no package installs.
#
# Why one file: the eleven differed only in base image, package list and final
# command, and the drift showed -- three pinned different Debian/Alpine tags for
# the same job. A stage that shares its base with another cannot drift from it.

# ---------------------------------------------------------------- bases -----
# A CACHED deps image for docker/linux.sh. Every ad-hoc `docker run` that
# apt-installs its own toolchain pays ~8 packages per run; this pays once.
# Superset of base-deb: adds liburing (io_uring backend) and coreutils (stdbuf,
# which is how a crash's last line survives a fully-buffered stdout).
# libzstd-dev/libbrotli-dev are CONFIGURATION PARITY: without them CONFIG_ZSTD
# and brotli auto-disable and a run silently tests a SMALLER engine than ships.
FROM debian:trixie-slim AS deps
ENV DEBIAN_FRONTEND=noninteractive
# The COMPLETE Linux toolchain for this project, built once and reused by
# docker/linux.sh. Deliberately a superset of what any single build needs:
#   BOTH compilers -- the Makefile defaults to gcc and takes clang via
#     CONFIG_CLANG=y; a missing gcc fails with "gcc: No such file or directory",
#     which reads as a broken Makefile rather than a missing package.
#   libclang-rt-dev -- clang's SANITIZER RUNTIMES. Without it an ASan build
#     fails at LINK, not at compile, so it looks like a code error.
#   gdb/valgrind -- a crash with no symbols is otherwise undiagnosable here.
#   liburing-dev  -- the io_uring backend exists only on Linux.
#   zip/unzip     -- TEST dependencies: test_archive SKIPS itself without them,
#                    and a skip reads as a pass in the summary line.
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      clang lld llvm libclang-rt-dev \
      gcc g++ make libc6-dev pkg-config \
      gdb valgrind binutils file \
      libsqlite3-dev libssl-dev liburing-dev zlib1g-dev \
      libzstd-dev libbrotli-dev \
      python3 bash coreutils git ca-certificates curl \
      zip unzip lz4 xz-utils \
      redis-server postgresql \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /work

# The db tests need redis + postgres RUNNING, not just installed; the binaries
# in this image are started by the test-runner invocation, not by the image:
#   redis-server --port 56379 --daemonize yes --save ''
#   pg_ctlcluster <ver> main start   # then: ALTER ROLE ... / trust in pg_hba
# (tests/test_db_lifecycle.js spawns redis itself; postgres must be up.)

# The same contract for musl: a CACHED toolchain the test runs never rebuild.
# Same package list as base-musl had -- clang, lld, the parity dev headers
# (sqlite/openssl/zstd/brotli -- CONFIGURATION PARITY, not extras: without them
# CONFIG_SQLITE/CONFIG_TLS/CONFIG_ZSTD/brotli auto-disable and a run silently
# tests a SMALLER engine than ships).
FROM alpine:3.21 AS deps-musl
RUN apk add --no-cache clang lld make musl-dev compiler-rt bash zip unzip lz4 python3 \
      sqlite-dev openssl-dev zstd-dev brotli-dev git curl
WORKDIR /work

FROM debian:trixie-slim AS base-deb
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      clang lld make libc6-dev pkgconf ca-certificates curl python3 bash zip unzip lz4 \
      libsqlite3-dev libssl-dev libzstd-dev libbrotli-dev git \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src

# ------------------------------------------------------------- io_uring -----
# liburing only exists here. Both backends are exercised, and the tests need
# --security-opt seccomp=unconfined because io_uring setup is filtered by the
# default profile.
FROM base-deb AS base-uring
RUN apt-get update \
 && apt-get install -y --no-install-recommends liburing-dev apache2-utils \
 && rm -rf /var/lib/apt/lists/*
COPY . .
# uring-tests/uring-bench run BOTH backends through /usr/local/bin/dyna-epoll
# and /usr/local/bin/dyna-uring, so both binaries are built here and installed
# where the scripts look (audit 13.5.1). Every config writes the same ./dynajs
# and ./libdynajs.a, so clean between the two (same idiom as the amd64 stage's
# ml-oracle block). The uring build adds CONFIG_IO_URING=y; the epoll build is
# the default backend.
RUN make clean >/dev/null \
 && make CONFIG_CLANG=y CONFIG_NATIVE_MODULES=y -j"$(nproc)" \
 && install -m755 dynajs /usr/local/bin/dyna-epoll \
 && make clean >/dev/null \
 && make CONFIG_CLANG=y CONFIG_NATIVE_MODULES=y CONFIG_IO_URING=y -j"$(nproc)" \
 && install -m755 dynajs /usr/local/bin/dyna-uring

FROM base-uring AS uring-tests
COPY docker/uring-tests.sh /usr/local/bin/uring-tests.sh
CMD ["/usr/local/bin/uring-tests.sh"]

FROM base-uring AS uring-bench
COPY docker/uring-bench.sh /usr/local/bin/uring-bench.sh
CMD ["/usr/local/bin/uring-bench.sh"]

FROM base-uring AS uring-app
COPY docker/uring-app-test.sh /usr/local/bin/uring-app-test.sh
CMD ["/usr/local/bin/uring-app-test.sh"]

# ------------------------------------------------------------ benchmarks ----
# Node is the comparison baseline, so these start from the node image rather
# than installing it.
FROM node:26-trixie-slim AS base-bench
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      clang lld make libc6-dev ca-certificates curl python3 bash \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN make CONFIG_CLANG=y CONFIG_NATIVE_MODULES=y -j"$(nproc)"

FROM base-bench AS bench
CMD ["bash", "/src/bench/bench_linux.sh"]

FROM base-bench AS profile
RUN apt-get update \
 && apt-get install -y --no-install-recommends wrk linux-perf \
 && rm -rf /var/lib/apt/lists/*
CMD ["bash", "/src/docker/profile-run.sh"]

# --------------------------------------------------------------- dev shell --
FROM base-deb AS dev
CMD ["bash"]
