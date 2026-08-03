# One multi-stage Dockerfile for every container gate. Replaces eleven
# near-identical files, five of which nothing referenced any more.
#
#   docker build --target <t> -f docker/Dockerfile .
#
#   amd64        the x86-64 proof gate: SIMD dispatch, full suite, ml oracle,
#                the hardware-CRC differential. --platform linux/amd64.
#   glibc        portability smoke test on glibc
#   musl         portability smoke test on musl
#   uring-tests  io_uring + port-binding tests, BOTH backends
#   uring-bench  io_uring throughput
#   bench        Linux benchmark run against Node
#   profile      perf/wrk profiling run
#   dev          interactive toolchain, no build
#
# Why one file: the eleven differed only in base image, package list and final
# command, and the drift showed -- three pinned different Debian/Alpine tags for
# the same job. A stage that shares its base with another cannot drift from it.

# ---------------------------------------------------------------- bases -----
# A CACHED deps image for docker/linux.sh. Every ad-hoc `docker run` that
# apt-installs its own toolchain pays ~8 packages per run; this pays once.
# Superset of base-deb: adds liburing (io_uring backend) and coreutils (stdbuf,
# which is how a crash's last line survives a fully-buffered stdout).
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
      python3 bash coreutils git ca-certificates curl \
      zip unzip lz4 xz-utils \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /work

FROM debian:trixie-slim AS base-deb
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      clang lld make libc6-dev ca-certificates curl python3 bash zip unzip lz4 \
      libsqlite3-dev libssl-dev git \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src

FROM alpine:3.21 AS base-musl
# zip/unzip are TEST DEPENDENCIES: test_archive's foreign-archive check skips
# itself without them, and a skip reads as a pass in the summary line.
# sqlite/openssl dev headers are CONFIGURATION PARITY, not extras: without them
# CONFIG_SQLITE and CONFIG_TLS auto-disable and the image silently gates a
# SMALLER engine than ships -- README ex16 (dyna:net SQLite) failed exactly so.
RUN apk add --no-cache clang lld make musl-dev compiler-rt bash zip unzip lz4 python3 \
      sqlite-dev openssl-dev git
WORKDIR /src

# ------------------------------------------------------------- amd64 gate ---
# The arm64 dev host cannot execute an x86 kernel, so anything ISA-specific is
# unrun until this stage runs it. Emulation reaches SSE4.2 but not AVX2, which
# is why the caps probe prints what actually dispatched rather than assuming.
FROM base-musl AS amd64
# A skipped foreign-tool oracle is not a pass: make test_archive fail rather
# than skip if zip/unzip are missing. (No trailing comment -- ENV takes the
# whole rest of the line as the value.)
ENV DYNAJS_REQUIRE_TOOLS=1
ENV QEMU_CPU=Haswell
ENV LDFLAGS="-g -fuse-ld=lld --rtlib=compiler-rt"
COPY . .
RUN make clean || true
RUN make CONFIG_CLANG=y CONFIG_NATIVE_MODULES=y -j"$(nproc)"
RUN printf '#include "dyna-simd-kernels.h"\n#include <stdio.h>\nint main(void){simd_init();unsigned long long c=cpu_features();printf("SIMD caps=0x%%llx  SSE42=%%d AVX2=%%d AVX512=%%d NEON=%%d\\n",c,!!(c&CPU_SSE42),!!(c&CPU_AVX2),!!(c&CPU_AVX512F),!!(c&CPU_NEON));return 0;}' > /tmp/caps.c \
 && clang -O2 -Isrc /tmp/caps.c .obj/dyna-simd-core.o .obj/dyna-simd-scalar.o \
      .obj/dyna-simd-sse42.o .obj/dyna-simd-avx2.o .obj/dyna-simd-avx512.o \
      .obj/dyna-simd-neon.o .obj/dyna-simd-sve.o $LDFLAGS -lpthread -lm -o /tmp/caps \
 && echo "=== x86 SIMD dispatch under qemu ===" && /tmp/caps
RUN echo "=== x86-64 vectorization audit (baseline, no -march) ===" \
 && bash tools/vecaudit.sh src/dyna-ml.c | sort -t= -k2 -rn | head -16 \
 && echo "--- libm calls per function (a call in a loop body blocks the vectoriser) ---" \
 && bash tools/libm-in-loops.sh src/dyna-ml.c
RUN make CONFIG_CLANG=y CONFIG_NATIVE_MODULES=y test
RUN make CONFIG_CLANG=y CONFIG_NATIVE_MODULES=y test-native
# The adversarial suites. They are the reason this stage is worth running on a
# second architecture at all: the parser, module, extension and net/file pen
# tests assert BOUNDS and REFUSALS, and those are exactly what differ between
# a 64-bit LP64 arm64 and an emulated x86-64 -- integer widths, alignment, and
# the SIMD kernels the reductions dispatch to.
RUN make CONFIG_CLANG=y CONFIG_NATIVE_MODULES=y test-security
RUN ./dynajs tests/test_simd.js && ./dynajs tests/test_simd_f64.js \
 && ./dynajs tests/test_simd_int.js
# Hardware checksum and digest paths against the bit-serial / published
# definitions, on the ISA whose instructions the dev host cannot execute.
# The thread-sanitizer gates are NOT here: TSan's shadow mapping is larger than
# user-mode emulation can allocate, and "cannot allocate memory" is not a finding.
RUN make CONFIG_CLANG=y test-crc32c-hw \
 && make CONFIG_CLANG=y test-sha256-hw
# The ml oracle, built twice and diffed: vectorised against strictly sequential.
RUN ./dynajs tests/test_ml_oracle.js > /tmp/ml_vec.txt \
 && make clean >/dev/null \
 && make CONFIG_CLANG=y CONFIG_NATIVE_MODULES=y CONFIG_ML_NO_SIMD=y -j"$(nproc)" >/dev/null \
 && ./dynajs tests/test_ml_oracle.js > /tmp/ml_seq.txt \
 && echo "=== dyna:ml oracle diff on x86-64 ===" \
 && ./dynajs tests/test_ml_oracle.js --diff /tmp/ml_vec.txt /tmp/ml_seq.txt
CMD ["/tmp/caps"]

# ------------------------------------------------------- portability smoke --
FROM base-deb AS glibc
COPY . .
RUN make CONFIG_CLANG=y CONFIG_NATIVE_MODULES=y -j"$(nproc)"
CMD ["./dynajs", "-e", "print('dynascript on glibc (' + (typeof Array.fromAsync) + ' fromAsync) OK')"]

FROM base-musl AS musl
COPY . .
RUN make CONFIG_CLANG=y CONFIG_NATIVE_MODULES=y -j"$(nproc)"
CMD ["./dynajs", "-e", "print('dynascript on musl (' + (typeof Array.fromAsync) + ' fromAsync) OK')"]

# ------------------------------------------------------------- io_uring -----
# liburing only exists here. Both backends are exercised, and the tests need
# --security-opt seccomp=unconfined because io_uring setup is filtered by the
# default profile.
FROM base-deb AS base-uring
RUN apt-get update \
 && apt-get install -y --no-install-recommends liburing-dev \
 && rm -rf /var/lib/apt/lists/*
COPY . .

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
