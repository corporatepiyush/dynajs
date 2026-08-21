#
# DynaJS Javascript Engine
#
# Copyright (c) 2017-2021 Fabrice Bellard
# Copyright (c) 2017-2021 Charlie Gordon
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.

ifeq ($(shell uname -s),Darwin)
CONFIG_DARWIN=y
endif
ifeq ($(shell uname -s),FreeBSD)
CONFIG_FREEBSD=y
endif
# Windows cross compilation from Linux
# May need to have libwinpthread*.dll alongside the executable
# (On Ubuntu/Debian may be installed with mingw-w64-x86-64-dev
# to /usr/x86_64-w64-mingw32/lib/libwinpthread-1.dll)
#CONFIG_WIN32=y
# use link time optimization (smaller and faster executables but slower build)
#CONFIG_LTO=y
# consider warnings as errors (for development)
#CONFIG_WERROR=y
# force 32 bit build on x86_64
#CONFIG_M32=y
# cosmopolitan build (see https://github.com/jart/cosmopolitan)
#CONFIG_COSMO=y

# installation directory
PREFIX?=/usr/local

# use the gprof profiler
#CONFIG_PROFILE=y
# use address sanitizer
#CONFIG_ASAN=y
# use memory sanitizer
#CONFIG_MSAN=y
# use UB sanitizer
#CONFIG_UBSAN=y
# use thread sanitizer
#CONFIG_TSAN=y

# TEST262 bootstrap config: commit id and shallow "since" parameter
TEST262_COMMIT?=5c8206929d81b2d3d727ca6aac56c18358c8d790
TEST262_SINCE?=2025-09-01

OBJDIR=.obj

ifdef CONFIG_ASAN
OBJDIR:=$(OBJDIR)/asan
endif
ifdef CONFIG_MSAN
OBJDIR:=$(OBJDIR)/msan
endif
ifdef CONFIG_UBSAN
OBJDIR:=$(OBJDIR)/ubsan
endif
ifdef CONFIG_TSAN
OBJDIR:=$(OBJDIR)/tsan
endif

# CONFIG_FASTDEV -- the edit/build/test knob, NOT a build you may ship or measure.
# The engine is one translation unit (dynajs.c includes 28 fragments), so it is
# 32% of the serial build and the whole build's Amdahl floor: -j4 and -j8 both
# land on it. Measured: that TU is 9.02s at -O2, 8.95s at -O1 and 1.43s at -O0,
# so there is no middle tier -- the optimiser's first pass is the entire cost.
# Its own OBJDIR, so an -O0 object can never be linked into an -O2 tree.
ifdef CONFIG_FASTDEV
OBJDIR:=$(OBJDIR)/fastdev
endif

ifdef CONFIG_DARWIN
# use clang instead of gcc
CONFIG_CLANG=y
CONFIG_DEFAULT_AR=y
endif
ifdef CONFIG_FREEBSD
# use clang instead of gcc
CONFIG_CLANG=y
CONFIG_DEFAULT_AR=y
CONFIG_LTO=
endif

ifdef CONFIG_WIN32
  ifdef CONFIG_M32
    CROSS_PREFIX?=i686-w64-mingw32-
  else
    CROSS_PREFIX?=x86_64-w64-mingw32-
  endif
  EXE=.exe
else ifdef MSYSTEM
  CONFIG_WIN32=y
  CROSS_PREFIX?=
  EXE=.exe
else
  CROSS_PREFIX?=
  EXE=
endif

ifdef CONFIG_CLANG
  HOST_CC=clang
  CC=$(CROSS_PREFIX)clang
  CFLAGS+=-g -Wall -MMD -MF $(OBJDIR)/$(@F).d
  CFLAGS += -Wextra
  CFLAGS += -Wno-sign-compare
  CFLAGS += -Wno-missing-field-initializers
  CFLAGS += -Wundef -Wuninitialized
  CFLAGS += -Wunused -Wno-unused-parameter
  CFLAGS += -Wwrite-strings
  CFLAGS += -Wchar-subscripts -funsigned-char
  CFLAGS += -MMD -MF $(OBJDIR)/$(@F).d
  ifdef CONFIG_DEFAULT_AR
    AR=$(CROSS_PREFIX)ar
  else
    ifdef CONFIG_LTO
      AR=$(CROSS_PREFIX)llvm-ar
    else
      AR=$(CROSS_PREFIX)ar
    endif
  endif
  LIB_FUZZING_ENGINE ?= "-fsanitize=fuzzer"

# A fuzz target with no sanitizer catches CRASHES only: a planted one-byte
# overread survived 5229 executions unreported (src/fuzz/README).
#
# fuzz_dyns/lz4/net/scram already hardcode -fsanitize=address,undefined. The
# seven that go through LIB_FUZZING_ENGINE did not, and they are the ones that
# missed the plant -- so a local build now gets one by DEFAULT, which makes all
# eleven consistent. OSS-Fuzz supplies its own via CFLAGS and sets
# FUZZ_NO_DEFAULT_SAN=y to opt out.
ifndef FUZZ_NO_DEFAULT_SAN
ifndef CONFIG_ASAN
ifndef CONFIG_MSAN
ifndef CONFIG_TSAN
FUZZ_DEFAULT_SAN = -fsanitize=address,undefined -fno-omit-frame-pointer
endif
endif
endif
endif
ifdef FUZZ_NO_DEFAULT_SAN
ifndef CONFIG_ASAN
ifndef CONFIG_MSAN
FUZZ_SAN_WARN = printf '\n  WARNING: built with NO memory sanitizer.\n\
  libFuzzer alone reports crashes only -- an out-of-bounds read produces\n\
  nothing. Drop FUZZ_NO_DEFAULT_SAN before trusting a clean run.\n\n'
endif
endif
endif
FUZZ_SAN_WARN ?= true
else ifdef CONFIG_COSMO
  CONFIG_LTO=
  HOST_CC=gcc
  CC=cosmocc
  # cosmocc does not correct support -MF
  CFLAGS=-g -Wall #-MMD -MF $(OBJDIR)/$(@F).d
  CFLAGS += -Wno-array-bounds -Wno-format-truncation
  AR=cosmoar
else
  HOST_CC=gcc
  CC=$(CROSS_PREFIX)gcc
  CFLAGS+=-g -Wall -MMD -MF $(OBJDIR)/$(@F).d
  CFLAGS += -Wno-array-bounds -Wno-format-truncation -Wno-infinite-recursion
  ifdef CONFIG_LTO
    AR=$(CROSS_PREFIX)gcc-ar
  else
    AR=$(CROSS_PREFIX)ar
  endif
endif
STRIP?=$(CROSS_PREFIX)strip
ifdef CONFIG_M32
CFLAGS+=-msse2 -mfpmath=sse # use SSE math for correct FP rounding
ifndef CONFIG_WIN32
CFLAGS+=-m32
LDFLAGS+=-m32
endif
endif
CFLAGS+=-std=gnu17 # pin C17 (gnu variant: computed-goto &&label / goto * need GNU extensions)
CFLAGS+=-fwrapv # ensure that signed overflows behave as expected
ifdef CONFIG_WERROR
CFLAGS+=-Werror
endif
DEFINES:=-D_GNU_SOURCE -DCONFIG_VERSION=\"$(shell cat VERSION)\"
ifdef CONFIG_WIN32
DEFINES+=-D__USE_MINGW_ANSI_STDIO # for standard snprintf behavior
endif
ifndef CONFIG_WIN32
ifeq ($(shell $(CC) -o /dev/null src/compat/test-closefrom.c 2>/dev/null && echo 1),1)
DEFINES+=-DHAVE_CLOSEFROM
endif
endif

CFLAGS+=$(DEFINES)
# -I. resolves dynajs.c's "src/*.inc.c" unity includes from the repo root;
# -Isrc resolves the project headers (dynajs.h, dyna-opcode.h, cutils.h, ...)
# now that all engine sources live under src/. VPATH lets the object rules find
# src/<x>.c for a flat $(OBJDIR)/<x>.o (generated repl.c etc. stay in the root,
# which make searches before VPATH).
CFLAGS+=-I. -Isrc
# src/core is the pure-C library layer (no JSValue/JSContext) shared by the
# engine and every native module -- see tools/core-purity.sh, which proves
# each core TU still builds with -Isrc/core ALONE. It is on VPATH so a core
# lands at a flat $(OBJDIR)/<x>.o like the rest.
VPATH=src:src/core:tools
CFLAGS_DEBUG=$(CFLAGS) -O0
CFLAGS_SMALL=$(CFLAGS) -Os
ifdef CONFIG_FASTDEV
CFLAGS_OPT=$(CFLAGS) -O0
else
CFLAGS_OPT=$(CFLAGS) -O2
endif
# opt-in local-CPU tuning for benchmarking; off by default so shipped builds
# stay portable. -mcpu=native picks up the host uarch (NEON/SVE widths, etc.).
ifdef CONFIG_NATIVE
CFLAGS_OPT+=-mcpu=native
CFLAGS_SMALL+=-mcpu=native
endif
# The per-shape property hash mix, ON BY DEFAULT (audit 2.3): it removes a
# quadratic blowup an attacker reaches through JSON.parse -- see
# prop_hash_bucket() in src/object/shapes_objects_gc.inc.c and
# tests/bench_prop_hash.js, which prints the difference either way. It costs
# ~0.3 ns per property lookup (flat on prop_read/prop_write/prop_update/
# array_read, +7% on global_write and the string->number rows, where a lookup
# is most of the work). Opt out with CONFIG_PROP_HASH_MIX=n for maximum lookup
# speed. NOTE: make tracks timestamps, not flags, so `make clean` after
# changing this or the objects are stale.
ifneq ($(CONFIG_PROP_HASH_MIX),n)
CFLAGS+=-DCONFIG_PROP_HASH_MIX
endif
CFLAGS_NOLTO:=$(CFLAGS_OPT)
ifdef CONFIG_COSMO
LDFLAGS+=-s # better to strip by default
else
LDFLAGS+=-g
endif
ifdef CONFIG_LTO
CFLAGS_SMALL+=-flto
CFLAGS_OPT+=-flto
LDFLAGS+=-flto
endif
ifdef CONFIG_PROFILE
CFLAGS+=-p
LDFLAGS+=-p
endif
# mimalloc v3 as the runtime allocator (opt-in experiment; vendored under
# third_party/mimalloc -- run: git clone --branch v3.1.5 \
# https://github.com/microsoft/mimalloc third_party/mimalloc). Tunables via the
# DYNA_MI_* / MIMALLOC_* env vars.
ifdef CONFIG_MIMALLOC
CFLAGS+=-DCONFIG_MIMALLOC -Ithird_party/mimalloc/include
endif
# Profile-guided optimization (clang PGO). Phase 1 instruments, phase 2 uses the
# merged profile. Use `make pgo` for the full flow; combine with CONFIG_LTO for
# the best result. (BOLT is a separate post-link step -- see the `bolt` note.)
ifdef CONFIG_PGO_GEN
CFLAGS+=-fprofile-generate=pgo-data
LDFLAGS+=-fprofile-generate=pgo-data
endif
ifdef CONFIG_PGO_USE
CFLAGS+=-fprofile-use=pgo.profdata -Wno-profile-instr-out-of-date -Wno-profile-instr-unprofiled -Wno-backend-plugin
LDFLAGS+=-fprofile-use=pgo.profdata
endif
ifdef CONFIG_ASAN
CFLAGS+=-fsanitize=address -fno-omit-frame-pointer
LDFLAGS+=-fsanitize=address -fno-omit-frame-pointer
endif
# dynajsc links user programs against libdynajs.a; if the archive is
# instrumented, its own link line needs the same flag or it fails with an
# undefined __asan_version_mismatch_check_*.
ifdef CONFIG_ASAN
export DYNAJSC_EXTRA_CFLAGS+=-fsanitize=address
endif
ifdef CONFIG_UBSAN
export DYNAJSC_EXTRA_CFLAGS+=-fsanitize=undefined
endif
ifdef CONFIG_TSAN
export DYNAJSC_EXTRA_CFLAGS+=-fsanitize=thread
endif
ifdef CONFIG_MSAN
CFLAGS+=-fsanitize=memory -fno-omit-frame-pointer
LDFLAGS+=-fsanitize=memory -fno-omit-frame-pointer
endif
ifdef CONFIG_UBSAN
CFLAGS+=-fsanitize=undefined -fno-omit-frame-pointer
LDFLAGS+=-fsanitize=undefined -fno-omit-frame-pointer
endif
ifdef CONFIG_TSAN
CFLAGS+=-fsanitize=thread -fno-omit-frame-pointer
LDFLAGS+=-fsanitize=thread -fno-omit-frame-pointer
endif
# in-repo native modules (self-contained, NO external deps). A family is active
# iff its dyna-<family>.c is present. No -I/-l into any external tree.
# TLS is LINKED, not vendored, and that is an INTERIM decision (design 16):
# option A is days where vendoring is weeks. CONFIG_TLS_BACKEND=vendored is the
# exit, and the CVE stream is the price of "for now".
# macOS /usr/bin/openssl is LibreSSL and Homebrew keeps OpenSSL keg-only, so a
# bare pkg-config resolves to the WRONG stack. The floor is 3.0 (1.1.1 is EOL
# and its API differs) and it is enforced HERE, not in a comment.
ifdef CONFIG_TLS
OPENSSL_PC:=$(shell brew --prefix openssl@3 2>/dev/null || brew --prefix openssl 2>/dev/null)/lib/pkgconfig
OPENSSL_CFLAGS:=$(shell PKG_CONFIG_PATH="$(OPENSSL_PC):$$PKG_CONFIG_PATH" pkg-config --cflags 'openssl >= 3.0' 2>/dev/null)
OPENSSL_LIBS:=$(shell PKG_CONFIG_PATH="$(OPENSSL_PC):$$PKG_CONFIG_PATH" pkg-config --libs 'openssl >= 3.0' 2>/dev/null)
ifeq ($(OPENSSL_LIBS),)
$(error CONFIG_TLS=y needs OpenSSL >= 3.0. Install it (brew install openssl@3, \
  apt-get install libssl-dev, apk add openssl-dev) or set PKG_CONFIG_PATH. \
  Note /usr/bin/openssl on macOS is LibreSSL and does not satisfy this.)
endif
CFLAGS+=-DCONFIG_TLS $(OPENSSL_CFLAGS)
# EXTRA_LIBS, not LDFLAGS: the link is `$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)`,
# so a -l in LDFLAGS lands BEFORE the objects. GNU ld drops a library whose
# symbols nothing has referenced yet; lld does not, so clang hid this.
EXTRA_LIBS+=$(OPENSSL_LIBS)
endif
ifdef CONFIG_NATIVE_MODULES
CFLAGS+=-DCONFIG_NATIVE_MODULES
ifneq ($(wildcard src/dyna-random.c),)
CFLAGS+=-DCONFIG_NATIVE_MODULE_RANDOM
endif
ifneq ($(wildcard src/dyna-compress.c),)
CFLAGS+=-DCONFIG_NATIVE_MODULE_COMPRESS
endif
ifneq ($(wildcard src/dyna-net.c),)
CFLAGS+=-DCONFIG_NATIVE_MODULE_NET
endif
# SQLite is LINKED, not vendored: the amalgamation is ~9 MB and the system copy
# is a real, patched, security-tracked build. Detected, never assumed -- macOS
# ships 3.51 while the client wants 3.53 features, so the version is reported at
# runtime rather than trusted at compile time.
# Homebrew keeps sqlite keg-only, so a bare pkg-config silently resolves to the
# OLDER system copy (3.51 on macOS). Prefer the keg when it is installed, and
# let PKG_CONFIG_PATH from the environment win over both.
SQLITE_PC:=$(shell brew --prefix sqlite 2>/dev/null)/lib/pkgconfig
SQLITE_CFLAGS:=$(shell PKG_CONFIG_PATH="$(SQLITE_PC):$$PKG_CONFIG_PATH" pkg-config --cflags sqlite3 2>/dev/null)
SQLITE_LIBS:=$(shell PKG_CONFIG_PATH="$(SQLITE_PC):$$PKG_CONFIG_PATH" pkg-config --libs sqlite3 2>/dev/null)
ifneq ($(SQLITE_LIBS),)
CFLAGS+=-DCONFIG_SQLITE $(SQLITE_CFLAGS)
# after the objects -- see the OPENSSL_LIBS note above
EXTRA_LIBS+=$(SQLITE_LIBS)
endif
# zstd: LINKED, not vendored. Apple's libcompression has NO COMPRESSION_ZSTD
# on the macOS 26 SDK (verified by a compile error), so pkg-config libzstd is
# the only route (SQLITE pattern). Absent -> zstd()/unzstd() throw a named
# "not compiled in" error and the tests skip loudly.
ZSTD_PC:=$(shell brew --prefix zstd 2>/dev/null)/lib/pkgconfig
ZSTD_CFLAGS:=$(shell PKG_CONFIG_PATH="$(ZSTD_PC):$$PKG_CONFIG_PATH" pkg-config --cflags libzstd 2>/dev/null)
ZSTD_LIBS:=$(shell PKG_CONFIG_PATH="$(ZSTD_PC):$$PKG_CONFIG_PATH" pkg-config --libs libzstd 2>/dev/null)
ifneq ($(ZSTD_LIBS),)
CFLAGS+=-DCONFIG_ZSTD $(ZSTD_CFLAGS)
EXTRA_LIBS+=$(ZSTD_LIBS)
endif
# brotli: libcompression COMPRESSION_BROTLI on macOS (system, since 12.0);
# libbrotli via pkg-config elsewhere. Not vendored: a CLI-compatible brotli
# decoder needs the full RFC 7932 static dictionary (~120 KB), the opposite
# of "small and stable".
ifeq ($(shell uname -s),Darwin)
EXTRA_LIBS+=-lcompression
else
BROTLI_PC:=$(shell brew --prefix brotli 2>/dev/null)/lib/pkgconfig
BROTLI_CFLAGS:=$(shell PKG_CONFIG_PATH="$(BROTLI_PC):$$PKG_CONFIG_PATH" pkg-config --cflags libbrotlienc libbrotlidec 2>/dev/null)
BROTLI_LIBS:=$(shell PKG_CONFIG_PATH="$(BROTLI_PC):$$PKG_CONFIG_PATH" pkg-config --libs libbrotlienc libbrotlidec 2>/dev/null)
ifneq ($(BROTLI_LIBS),)
CFLAGS+=-DCONFIG_BROTLI $(BROTLI_CFLAGS)
EXTRA_LIBS+=$(BROTLI_LIBS)
endif
endif
ifneq ($(wildcard src/dyna-structures.c),)
CFLAGS+=-DCONFIG_NATIVE_MODULE_STRUCTURES
endif
ifneq ($(wildcard src/dyna-structures3.c),)
CFLAGS+=-DCONFIG_NATIVE_MODULE_STRUCTURES3
endif
ifneq ($(wildcard src/dyna-ml.c),)
CFLAGS+=-DCONFIG_NATIVE_MODULE_ML
endif
ifneq ($(wildcard src/dyna-simd.c),)
CFLAGS+=-DCONFIG_NATIVE_MODULE_SIMD
endif
ifneq ($(wildcard src/dyna-file.c),)
CFLAGS+=-DCONFIG_NATIVE_MODULE_FILE
endif
ifneq ($(wildcard src/dyna-semver.c),)
CFLAGS+=-DCONFIG_NATIVE_MODULE_SEMVER
endif
ifneq ($(wildcard src/dyna-bytes.c),)
CFLAGS+=-DCONFIG_NATIVE_MODULE_BYTES
endif
ifneq ($(wildcard src/dyna-crypto.c),)
CFLAGS+=-DCONFIG_NATIVE_MODULE_CRYPTO
endif
ifneq ($(wildcard src/dyna-matcher.c),)
CFLAGS+=-DCONFIG_NATIVE_MODULE_MATCHER
endif
ifneq ($(wildcard src/dyna-encoding.c),)
CFLAGS+=-DCONFIG_NATIVE_MODULE_ENCODING
endif
ifneq ($(wildcard src/dyna-time.c),)
CFLAGS+=-DCONFIG_NATIVE_MODULE_TIME
endif
ifneq ($(wildcard src/dyna-mathx.c),)
CFLAGS+=-DCONFIG_NATIVE_MODULE_MATHX
endif
ifneq ($(wildcard src/dyna-csv.c),)
CFLAGS+=-DCONFIG_NATIVE_MODULE_CSV
endif
ifneq ($(wildcard src/dyna-dataframe.c),)
CFLAGS+=-DCONFIG_NATIVE_MODULE_DATAFRAME
endif
ifneq ($(wildcard src/dyna-uuid.c),)
CFLAGS+=-DCONFIG_NATIVE_MODULE_UUID
endif
ifneq ($(wildcard src/dyna-config.c),)
CFLAGS+=-DCONFIG_NATIVE_MODULE_CONFIG
endif
ifneq ($(wildcard src/dyna-log.c),)
CFLAGS+=-DCONFIG_NATIVE_MODULE_LOG
endif
ifneq ($(wildcard src/dyna-url.c),)
CFLAGS+=-DCONFIG_NATIVE_MODULE_URL
endif
ifneq ($(wildcard src/dyna-term.c),)
CFLAGS+=-DCONFIG_NATIVE_MODULE_TERM
endif
ifneq ($(wildcard src/dyna-validate.c),)
CFLAGS+=-DCONFIG_NATIVE_MODULE_VALIDATE
endif
ifneq ($(wildcard src/dyna-json.c),)
CFLAGS+=-DCONFIG_NATIVE_MODULE_JSON
endif
ifneq ($(wildcard src/dyna-schema.c),)
CFLAGS+=-DCONFIG_NATIVE_MODULE_SCHEMA
endif
ifneq ($(wildcard src/dyna-xml.c),)
CFLAGS+=-DCONFIG_NATIVE_MODULE_XML
endif
ifneq ($(wildcard src/dyna-yaml.c),)
CFLAGS+=-DCONFIG_NATIVE_MODULE_YAML
endif
ifneq ($(wildcard src/dyna-decimal.c),)
CFLAGS+=-DCONFIG_NATIVE_MODULE_DECIMAL
endif
ifneq ($(wildcard src/dyna-vserialize.c),)
CFLAGS+=-DCONFIG_NATIVE_MODULE_VSERIALIZE
endif
ifneq ($(wildcard src/dyna-html.c),)
CFLAGS+=-DCONFIG_NATIVE_MODULE_HTML
endif
ifneq ($(wildcard src/dyna-sys.c),)
CFLAGS+=-DCONFIG_NATIVE_MODULE_SYS
endif
ifneq ($(wildcard src/dyna-scrape.c),)
CFLAGS+=-DCONFIG_NATIVE_MODULE_SCRAPE
endif
# Differential-oracle build for dyna:ml: drop every SIMD kernel call so the
# module reduces to a purely sequential model of the same algorithms. Compare the
# two builds with tests/test_ml_oracle.js (dump both, then --diff). Never ship.
ifdef CONFIG_ML_NO_SIMD
CFLAGS+=-DDYN_ML_NO_SIMD
endif
endif
ifdef CONFIG_WIN32
LDEXPORT=
else
LDEXPORT=-rdynamic
endif

ifndef CONFIG_COSMO
ifndef CONFIG_DARWIN
ifndef CONFIG_WIN32
CONFIG_SHARED_LIBS=y # building shared libraries is supported
endif
endif
endif

PROGS=dynajs$(EXE) dynajsc$(EXE) run-test262$(EXE)

# THE CONFIGURATION IS NOT A DEPENDENCY MAKE CAN SEE. Changing CONFIG_* changes
# the compiler flags and not one timestamp, so the next build reuses objects
# compiled with the old flags: the feature silently vanishes, and it is reported
# by a module nobody edited. This stamp covers the flags that SHARE an OBJDIR.
# Evaluated at parse time so it cannot race a parallel build.
#
# A PRIVATE OBJDIR PROTECTS THE OBJECTS, NOT THE BINARY, and this comment used
# to claim the sanitizers were therefore safe. They are not: $(PROGS) is one
# path shared by every configuration, so after building a variant the binary is
# NEWER than the incoming variant's objects and make links nothing -- measured,
# an -O0 FASTDEV build left the -O2 binary in place byte-for-byte. The variant
# stamp below is the other half, and it deletes only the binary so each
# variant's object cache survives.
CONFIG_SIG:=nm=$(CONFIG_NATIVE_MODULES) tls=$(CONFIG_TLS) sq=$(CONFIG_SQLITE) ol=$(CONFIG_OPENLIBM) mi=$(CONFIG_MIMALLOC) nat=$(CONFIG_NATIVE) lto=$(CONFIG_LTO) pgo=$(CONFIG_PGO_GEN)$(CONFIG_PGO_USE) iur=$(CONFIG_IO_URING) mlno=$(CONFIG_ML_NO_SIMD) phm=$(CONFIG_PROP_HASH_MIX) pro=$(CONFIG_PROFILE) m32=$(CONFIG_M32) cc=$(CONFIG_CLANG) cosmo=$(CONFIG_COSMO) win=$(CONFIG_WIN32)
CONFIG_SIG_FILE:=$(OBJDIR)/.config-sig
# Only when make is actually going to BUILD. A test or lint target is invoked
# without the CONFIG_* the binary was built with, so wiping there destroys the
# very binary the target is about to run.
# The archives are NOT in $(PROGS) yet -- `PROGS+=libdynajs.a` runs further
# down and both wipes below are `:=`, so naming them here is what makes a
# variant switch drop them. Without it a sanitizer archive poisons the next
# plain link, which surfaces as undefined __asan_* in an unrelated example.
ARCHIVES:=libdynajs.a libdynajs.lto.a libdynajs.fuzz.a
CFG_BUILD_GOALS:=all $(PROGS) $(ARCHIVES)
CFG_CHECK:=$(if $(MAKECMDGOALS),$(filter $(CFG_BUILD_GOALS),$(MAKECMDGOALS)),yes)
IGNORE_CFG:=$(if $(CFG_CHECK),$(shell \
  if [ ! -f "$(CONFIG_SIG_FILE)" ] || \
     [ "`cat "$(CONFIG_SIG_FILE)" 2>/dev/null`" != "$(CONFIG_SIG)" ]; then \
    rm -rf "$(OBJDIR)" $(PROGS) $(ARCHIVES) >/dev/null 2>&1; \
    mkdir -p "$(OBJDIR)" && printf '%s' '$(CONFIG_SIG)' > "$(CONFIG_SIG_FILE)"; \
  fi))

# Which VARIANT produced ./dynajs. Each variant keeps its own OBJDIR and so its
# own object cache; only the shared binary is removed, which costs one link.
# DO NOT VERIFY THIS BY HASHING THE BINARY. A switch relinks libdynajs.a and
# `ar` embeds mtimes, so two builds of identical code hash differently -- the
# same trap as LC_UUID. Probe BEHAVIOUR: an -O0 engine runs a CPU-bound loop
# about 5x slower than -O2 (measured 343 ms vs 65 ms), which is unambiguous.
VARIANT_SIG:=objdir=$(OBJDIR)
VARIANT_SIG_FILE:=.build-variant
IGNORE_VARIANT:=$(if $(CFG_CHECK),$(shell \
  if [ "`cat "$(VARIANT_SIG_FILE)" 2>/dev/null`" != "$(VARIANT_SIG)" ]; then \
    rm -f $(PROGS) $(ARCHIVES) >/dev/null 2>&1; \
    printf '%s' '$(VARIANT_SIG)' > "$(VARIANT_SIG_FILE)"; \
  fi))

ifneq ($(CROSS_PREFIX),)
DYNAJSC_CC=gcc
DYNAJSC=./host-dynajsc
PROGS+=$(DYNAJSC)
else
DYNAJSC_CC=$(CC)
DYNAJSC=./dynajsc$(EXE)
endif
PROGS+=libdynajs.a
ifdef CONFIG_LTO
PROGS+=libdynajs.lto.a
endif

# examples
ifeq ($(CROSS_PREFIX),)
ifndef CONFIG_ASAN
ifndef CONFIG_MSAN
ifndef CONFIG_UBSAN
PROGS+=examples/hello examples/test_fib
# no -m32 option in dynajsc
ifndef CONFIG_M32
ifndef CONFIG_WIN32
PROGS+=examples/hello_module
endif
endif
ifdef CONFIG_SHARED_LIBS
PROGS+=examples/fib.so examples/point.so
endif
endif
endif
endif
endif

# Command-only targets are PHONY: a stray file with one of these names (e.g. a
# leftover `prepush`) would otherwise make the gate silently "up to date"
# (audit 13.5.3). test-tls/test-aio-tls build a real binary AND run it, so
# without this an up-to-date binary skips the run on every later invocation.
# The fuzz_* names and libdynajs*.a are REAL outputs and stay out on purpose.
.PHONY: all clean install install-hooks check-hooks \
	prepush conformance sbom pgo stats microbench bench-core bench-core-tus \
	oracle-dtoa oracle-regexp api-inventory bolt-help \
	test test-native test-api test-security test-examples test-repl \
	test-uring test-nofile test-pool test-aio-disk test-aio-tls \
	test-io-atomic test-dns-codec test-resp-codec test-scram test-timer \
	test-regexp-prefilter test-dtoa-subnormal test-simd-bitmap test-ds-core \
	test-crc32c-hw test-crc32c-race test-sha256-hw \
	test-tls test-tls-conn test-x509 test-tls-server test-http-tls \
	test-crypto-aead test-crypto-curve test-jwt-asym \
	test-static-traversal test-html-pentest test-bytes-accessors \
	test-crawl test-fetcher test-connect-resolve \
	check-readme check-install check-api check-error-ids check-anchors \
	fuzz-audit fuzz-all fuzz-smoke libfuzzer tls-link-audit \
	test2 test2-default test2-update test2-check test2-bootstrap

all: $(OBJDIR) $(OBJDIR)/dynajs.check.o $(OBJDIR)/dyna-cli.check.o $(PROGS)

# src/core: pure-C libraries with no engine dependency. They live in
# libdynajs.a so the engine, the CLI and every native module share ONE audited
# implementation; a build that references none of them pulls nothing from the
# archive, so the cost of an unused core is zero.
CORE_OBJS=$(OBJDIR)/dyn-hash.o $(OBJDIR)/dyn-codec.o $(OBJDIR)/dyn-prng.o $(OBJDIR)/dyn-compress.o $(OBJDIR)/dyn-ds.o $(OBJDIR)/dyn-serial.o \
          $(OBJDIR)/dyn-path.o $(OBJDIR)/dyn-mathx.o $(OBJDIR)/dyn-ac.o $(OBJDIR)/dyn-dict.o \
          $(OBJDIR)/dyn-pool.o $(OBJDIR)/dyn-timer.o $(OBJDIR)/dyn-dns.o $(OBJDIR)/dyn-resp.o $(OBJDIR)/dyn-scram.o \
          $(OBJDIR)/dyn-snappy.o

DYNAJS_LIB_OBJS=$(OBJDIR)/dynajs.o $(OBJDIR)/dtoa.o $(OBJDIR)/libregexp.o $(OBJDIR)/libunicode.o $(OBJDIR)/cutils.o $(OBJDIR)/dyna-libc.o $(OBJDIR)/dyna-io.o $(CORE_OBJS) $(OBJDIR)/dyna-simd-core.o $(OBJDIR)/dyna-simd-scalar.o $(OBJDIR)/dyna-simd-neon.o $(OBJDIR)/dyna-simd-sse42.o $(OBJDIR)/dyna-simd-avx2.o $(OBJDIR)/dyna-simd-avx512.o $(OBJDIR)/dyna-simd-sve.o

ifdef CONFIG_MIMALLOC
DYNAJS_LIB_OBJS+=$(OBJDIR)/mimalloc.o
endif

DYNAJS_OBJS=$(OBJDIR)/dyna-cli.o $(OBJDIR)/repl.o $(DYNAJS_LIB_OBJS)
ifdef CONFIG_NATIVE_MODULES
# in-repo native module objects (framework + each present family)
NAT_MODULE_OBJS=$(OBJDIR)/dyna-nat.o
# shared async IO adapter + readiness reactor (used by http and the io engine).
# dyna-aio.c is the portable readiness backend; dyna-aio-uring.c is the Linux
# io_uring backend (self-guarded, empty unless CONFIG_IO_URING). One provides the
# dyn_aio_* symbols; the other compiles to nothing.
NAT_MODULE_OBJS+=$(OBJDIR)/dyna-aio.o $(OBJDIR)/dyna-aio-uring.o $(OBJDIR)/dyna-evloop.o
ifneq ($(wildcard src/dyna-random.c),)
NAT_MODULE_OBJS+=$(OBJDIR)/dyna-random.o
endif
ifneq ($(wildcard src/dyna-compress.c),)
NAT_MODULE_OBJS+=$(OBJDIR)/dyna-compress.o
endif
ifneq ($(wildcard src/dyna-net.c),)
# The TLS adapter belongs to the net module, so it is listed where net is.
ifdef CONFIG_TLS
NAT_MODULE_OBJS+=$(OBJDIR)/dyna-tls.o
endif
NAT_MODULE_OBJS+=$(OBJDIR)/dyna-net.o $(OBJDIR)/dyna-http.o \
                 $(OBJDIR)/dyna-netip.o $(OBJDIR)/dyna-net-tcp.o \
                 $(OBJDIR)/dyna-net-proxy.o \
                 $(OBJDIR)/dyna-net-dns.o $(OBJDIR)/dyna-net-redis.o \
                 $(OBJDIR)/dyna-net-pg.o $(OBJDIR)/dyna-net-ratelimit.o $(OBJDIR)/dyna-net-metrics.o
ifneq ($(SQLITE_LIBS),)
NAT_MODULE_OBJS+=$(OBJDIR)/dyna-net-sqlite.o
endif
endif
ifneq ($(wildcard src/dyna-structures.c),)
NAT_MODULE_OBJS+=$(OBJDIR)/dyna-structures.o $(OBJDIR)/dyna-serialize.o \
                 $(OBJDIR)/dyna-graph.o
endif
ifneq ($(wildcard src/dyna-structures3.c),)
NAT_MODULE_OBJS+=$(OBJDIR)/dyna-structures3.o
endif
ifneq ($(wildcard src/dyna-ml.c),)
NAT_MODULE_OBJS+=$(OBJDIR)/dyna-ml.o
endif
ifneq ($(wildcard src/dyna-simd.c),)
NAT_MODULE_OBJS+=$(OBJDIR)/dyna-simd.o
endif
ifneq ($(wildcard src/dyna-file.c),)
NAT_MODULE_OBJS+=$(OBJDIR)/dyna-file.o
endif
ifneq ($(wildcard src/dyna-semver.c),)
NAT_MODULE_OBJS+=$(OBJDIR)/dyna-semver.o
endif
ifneq ($(wildcard src/dyna-bytes.c),)
NAT_MODULE_OBJS+=$(OBJDIR)/dyna-bytes.o
endif
ifneq ($(wildcard src/dyna-crypto.c),)
NAT_MODULE_OBJS+=$(OBJDIR)/dyna-crypto.o
endif
ifneq ($(wildcard src/dyna-matcher.c),)
NAT_MODULE_OBJS+=$(OBJDIR)/dyna-matcher.o
endif
ifneq ($(wildcard src/dyna-encoding.c),)
NAT_MODULE_OBJS+=$(OBJDIR)/dyna-encoding.o
endif
ifneq ($(wildcard src/dyna-time.c),)
NAT_MODULE_OBJS+=$(OBJDIR)/dyna-time.o
endif
ifneq ($(wildcard src/dyna-mathx.c),)
NAT_MODULE_OBJS+=$(OBJDIR)/dyna-mathx.o
endif
ifneq ($(wildcard src/dyna-csv.c),)
NAT_MODULE_OBJS+=$(OBJDIR)/dyna-csv.o
endif
ifneq ($(wildcard src/dyna-dataframe.c),)
NAT_MODULE_OBJS+=$(OBJDIR)/dyna-dataframe.o
endif
ifneq ($(wildcard src/dyna-uuid.c),)
NAT_MODULE_OBJS+=$(OBJDIR)/dyna-uuid.o
endif
ifneq ($(wildcard src/dyna-config.c),)
NAT_MODULE_OBJS+=$(OBJDIR)/dyna-config.o
endif
ifneq ($(wildcard src/dyna-log.c),)
NAT_MODULE_OBJS+=$(OBJDIR)/dyna-log.o
endif
ifneq ($(wildcard src/dyna-url.c),)
NAT_MODULE_OBJS+=$(OBJDIR)/dyna-url.o
endif
ifneq ($(wildcard src/dyna-term.c),)
NAT_MODULE_OBJS+=$(OBJDIR)/dyna-term.o
endif
ifneq ($(wildcard src/dyna-validate.c),)
NAT_MODULE_OBJS+=$(OBJDIR)/dyna-validate.o
endif
ifneq ($(wildcard src/dyna-json.c),)
NAT_MODULE_OBJS+=$(OBJDIR)/dyna-json.o
endif
ifneq ($(wildcard src/dyna-schema.c),)
NAT_MODULE_OBJS+=$(OBJDIR)/dyna-schema.o
endif
ifneq ($(wildcard src/dyna-xml.c),)
NAT_MODULE_OBJS+=$(OBJDIR)/dyna-xml.o
endif
ifneq ($(wildcard src/dyna-yaml.c),)
NAT_MODULE_OBJS+=$(OBJDIR)/dyna-yaml.o
endif
ifneq ($(wildcard src/dyna-decimal.c),)
NAT_MODULE_OBJS+=$(OBJDIR)/dyna-decimal.o
endif
ifneq ($(wildcard src/dyna-vserialize.c),)
NAT_MODULE_OBJS+=$(OBJDIR)/dyna-vserialize.o
endif
ifneq ($(wildcard src/dyna-protobuf.c),)
NAT_MODULE_OBJS+=$(OBJDIR)/dyna-protobuf.o
endif
ifneq ($(wildcard src/dyna-asn1.c),)
NAT_MODULE_OBJS+=$(OBJDIR)/dyna-asn1.o
endif
ifneq ($(wildcard src/dyna-html.c),)
NAT_MODULE_OBJS+=$(OBJDIR)/dyna-html.o
endif
ifneq ($(wildcard src/dyna-sys.c),)
NAT_MODULE_OBJS+=$(OBJDIR)/dyna-sys.o
ifneq ($(wildcard src/dyna-scrape.c),)
NAT_MODULE_OBJS+=$(OBJDIR)/dyna-scrape.o
endif
endif
ifdef CONFIG_IO_URING
ifneq ($(wildcard src/dyna-uring.c),)
NAT_MODULE_OBJS+=$(OBJDIR)/dyna-uring.o
endif
endif
DYNAJS_OBJS+=$(NAT_MODULE_OBJS)
endif

HOST_LIBS=-lm -ldl -lpthread
LIBS=-lm -lpthread
ifndef CONFIG_WIN32
LIBS+=-ldl
endif
LIBS+=$(EXTRA_LIBS)

# OpenLibm (JuliaMath) as the math backend for cross-platform BIT-REPRODUCIBLE
# Math.* / libm results -- the same reason Julia bundles it. Opt-in experiment;
# vendored under third_party/openlibm -- run:
#   git clone https://github.com/JuliaMath/openlibm third_party/openlibm \
#     && $(MAKE) -C third_party/openlibm
# It provides the standard C symbol names (sin/cos/pow/...), so prepending its
# static archive before -lm makes the linker resolve libm from it -- NO engine
# code change. Default build is untouched. See CLAUDE.md "Deterministic libm".
#
# AUTO-DETECTED as of 2026-07-26: if the vendored archive is present it is used
# by default, because the libm choice is OBSERVABLE in results, not just in
# speed. Measured: 15 of 156 lines of tests/test_ml_oracle.js differ between
# macOS system libm and openlibm (e.g. a logreg probability 2.2462102281623253e-127
# vs ...257e-127), so the same model on the same data answers differently per
# platform. Set CONFIG_OPENLIBM=n to force the system libm.
#
# Cost, measured on this host: openlibm's exp is SLOWER and its log is FASTER --
# logreg.fit 0.85-0.87x, gmm.fit 1.03-1.23x, trees 1.00x. Reproducibility is
# worth that; if a workload is exp-bound and reproducibility does not matter,
# CONFIG_OPENLIBM=n gets the speed back.
OPENLIBM_DIR?=third_party/openlibm
ifeq ($(CONFIG_OPENLIBM),n)
  # explicitly disabled
else ifneq ($(wildcard $(OPENLIBM_DIR)/libopenlibm.a),)
CFLAGS+=-DCONFIG_OPENLIBM -I$(OPENLIBM_DIR)/include
LIBS:=$(OPENLIBM_DIR)/libopenlibm.a $(LIBS)
HOST_LIBS:=$(OPENLIBM_DIR)/libopenlibm.a $(HOST_LIBS)
else ifdef CONFIG_OPENLIBM
$(error CONFIG_OPENLIBM=y but $(OPENLIBM_DIR)/libopenlibm.a is missing. Run: \
  git clone https://github.com/JuliaMath/openlibm $(OPENLIBM_DIR) && $(MAKE) -C $(OPENLIBM_DIR))
endif

# Opt-in io_uring backend for the dynajs:http async reactor (Linux only; needs
# liburing-dev). Falls back to epoll when unset. No effect on non-Linux hosts.
ifdef CONFIG_IO_URING
CFLAGS+=-DCONFIG_IO_URING
LIBS+=-luring
HOST_LIBS+=-luring
endif

# A directory target is only ever tested for EXISTENCE, so this rule cannot
# notice that .obj is there while .obj/examples is not -- and that is exactly
# what a sanitizer build leaves behind, because OBJDIR is .obj/asan and making
# it creates .obj as a side effect. `make clean && make CONFIG_ASAN=y && make
# test` then failed with "unable to open output file .obj/examples/fib.o".
# The recipes below create $(@D) themselves, which is idempotent and cannot
# get out of step with the target being built.
$(OBJDIR):
	mkdir -p $(OBJDIR) $(OBJDIR)/examples $(OBJDIR)/tests

dynajs$(EXE): $(DYNAJS_OBJS)
	$(CC) $(LDFLAGS) $(LDEXPORT) -o $@ $^ $(LIBS)
ifneq ($(OBJDIR),.obj)
	@mkdir -p $(OBJDIR) && cp -f $@ $(OBJDIR)/$@ 2>/dev/null || true
endif

$(OBJDIR)/dynajs$(EXE): $(DYNAJS_OBJS)
	$(CC) $(LDFLAGS) $(LDEXPORT) -o $@ $^ $(LIBS)

dyna-debug$(EXE): $(patsubst %.o, %.debug.o, $(DYNAJS_OBJS))
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

dynajsc$(EXE): $(OBJDIR)/dynajsc.o $(DYNAJS_LIB_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)
ifneq ($(OBJDIR),.obj)
	@mkdir -p $(OBJDIR) && cp -f $@ $(OBJDIR)/$@ 2>/dev/null || true
endif

$(OBJDIR)/dynajsc$(EXE): $(OBJDIR)/dynajsc.o $(DYNAJS_LIB_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

fuzz_eval: $(OBJDIR)/fuzz_eval.o $(OBJDIR)/fuzz_common.o libdynajs.fuzz.a
	$(CC) $(CFLAGS_OPT) $^ -o fuzz_eval $(FUZZ_DEFAULT_SAN) $(LIB_FUZZING_ENGINE)

fuzz_compile: $(OBJDIR)/fuzz_compile.o $(OBJDIR)/fuzz_common.o libdynajs.fuzz.a
	$(CC) $(CFLAGS_OPT) $^ -o fuzz_compile $(FUZZ_DEFAULT_SAN) $(LIB_FUZZING_ENGINE)

# lre_exec calls simd_init for the prefilter kernel, so this target links the
# SIMD objects even though it links none of the engine.

SIMD_SRCS=src/dyna-simd-core.c src/dyna-simd-scalar.c src/dyna-simd-neon.c \
  src/dyna-simd-sse42.c src/dyna-simd-avx2.c src/dyna-simd-avx512.c src/dyna-simd-sve.c
SIMD_FUZZ_OBJS=$(patsubst src/%.c,$(OBJDIR)/%.fuzz.o,$(SIMD_SRCS))
fuzz_regexp: $(OBJDIR)/fuzz_regexp.o $(OBJDIR)/libregexp.fuzz.o $(OBJDIR)/cutils.fuzz.o $(OBJDIR)/libunicode.fuzz.o $(SIMD_FUZZ_OBJS)
	$(CC) $(CFLAGS_OPT) $^ -o fuzz_regexp $(FUZZ_DEFAULT_SAN) $(LIB_FUZZING_ENGINE)

# Mirrors fuzz_json: engine-linked, does not use fuzz_common's
# test_one_input_init. Its source shipped with NO rule and so had never been
# built -- which is why the uninitialised valid_flags[] read survived.
fuzz_regexp_compile: $(OBJDIR)/fuzz_regexp_compile.o libdynajs.fuzz.a
	$(CC) $(CFLAGS_OPT) $^ -o fuzz_regexp_compile $(FUZZ_DEFAULT_SAN) $(LIB_FUZZING_ENGINE)

# base32 through the public codec entry points. Same shape as fuzz_net: core
# only, no engine, and $(SIMD_SRCS) because dyn-codec.c dispatches through the
# SIMD table -- see the simd_init() note in the target.
fuzz_codec: src/fuzz/fuzz_codec.c src/core/dyn-codec.c $(SIMD_SRCS)
	$(CC) -O1 -g -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer \
	  -Isrc/core -Isrc -o fuzz_codec $^

# CSV parser, driven directly rather than through the JS class: csv_parse is
# static and only reachable via a file path, so the target includes the TU (see
# its header). dyna-nat.c supplies the three dyn_res_* symbols dyna-csv.c needs;
# everything else comes from the engine archive. NOT the nat archive -- that
# would duplicate every symbol the include already defines.
ifdef CONFIG_NATIVE_MODULES
fuzz_csv: src/fuzz/fuzz_csv.c src/dyna-nat.c libdynajs.fuzz.a
	$(CC) -O1 -g -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer \
	  -I. -Isrc -Isrc/core -DCONFIG_NATIVE_MODULES -DCONFIG_NATIVE_MODULE_CSV \
	  -o fuzz_csv $^
else
fuzz_csv:
	@echo "fuzz_csv needs CONFIG_NATIVE_MODULES=y: dyna-csv.c is gated on"
	@echo "CONFIG_NATIVE_MODULE_CSV and compiles to nothing without it."
	@exit 1
endif

# reader targets drive JS_ParseJSON / JS_ReadObject on the raw buffer; they
# do not use fuzz_common's test_one_input_init
fuzz_json: $(OBJDIR)/fuzz_json.o libdynajs.fuzz.a
	$(CC) $(CFLAGS_OPT) $^ -o fuzz_json $(FUZZ_DEFAULT_SAN) $(LIB_FUZZING_ENGINE)

fuzz_bytecode: $(OBJDIR)/fuzz_bytecode.o libdynajs.fuzz.a
	$(CC) $(CFLAGS_OPT) $^ -o fuzz_bytecode $(FUZZ_DEFAULT_SAN) $(LIB_FUZZING_ENGINE)

# The stdlib text parsers live in NAT_MODULE_OBJS, which libdynajs.fuzz.a does
# NOT contain -- a target linked against that archive compiles and fuzzes
# nothing, which reads exactly like a clean run.
ifdef CONFIG_NATIVE_MODULES
libdynajs-nat.fuzz.a: $(patsubst %.o, %.fuzz.o, $(DYNAJS_LIB_OBJS) $(NAT_MODULE_OBJS))
	$(AR) rcs $@ $^

# dyna-dataframe.c is in NAT_MODULE_OBJS too, so this needs the SAME archive:
# linked against libdynajs.fuzz.a it would compile and fuzz nothing.
fuzz_dataframe: $(OBJDIR)/fuzz_dataframe.o libdynajs-nat.fuzz.a
	$(CC) $(CFLAGS_OPT) $^ -o fuzz_dataframe $(FUZZ_DEFAULT_SAN) $(LIB_FUZZING_ENGINE) $(LIBS) $(SQLITE_LIBS)
	@$(FUZZ_SAN_WARN)

fuzz_stdlib: $(OBJDIR)/fuzz_stdlib.o libdynajs-nat.fuzz.a
	$(CC) $(CFLAGS_OPT) $^ -o fuzz_stdlib $(FUZZ_DEFAULT_SAN) $(LIB_FUZZING_ENGINE) $(LIBS) $(SQLITE_LIBS)
	@$(FUZZ_SAN_WARN)

# The pypi-plan parsers (IDNA/punycode, TOML, protobuf, ASN.1, JSON Schema,
# multipart, crypto KDFs) -- plan section 6 rule 7: fuzz target day one.
fuzz_parsers: $(OBJDIR)/fuzz_parsers.o libdynajs-nat.fuzz.a
	$(CC) $(CFLAGS_OPT) $^ -o fuzz_parsers $(FUZZ_DEFAULT_SAN) $(LIB_FUZZING_ENGINE) $(LIBS) $(SQLITE_LIBS)
	@$(FUZZ_SAN_WARN)
else
fuzz_dataframe:
	@echo "fuzz_dataframe needs CONFIG_NATIVE_MODULES=y: dyna-dataframe.c is in"
	@echo "NAT_MODULE_OBJS, and a default build compiles none of it."
	@exit 1

fuzz_stdlib:
	@echo "fuzz_stdlib needs CONFIG_NATIVE_MODULES=y: the parsers it covers are"
	@echo "in NAT_MODULE_OBJS, and a default build compiles none of them."
	@exit 1

fuzz_parsers:
	@echo "fuzz_parsers needs CONFIG_NATIVE_MODULES=y: the parsers it covers are"
	@echo "in NAT_MODULE_OBJS, and a default build compiles none of them."
	@exit 1
endif

fuzz_module_export: $(OBJDIR)/fuzz_module_export.o libdynajs.fuzz.a
	$(CC) $(CFLAGS_OPT) $^ -o fuzz_module_export $(FUZZ_DEFAULT_SAN) $(LIB_FUZZING_ENGINE)

# The DYNS record reader, built with the sanitizers that actually catch the bug
# class it defends against. The other targets use fuzzer-no-link objects, which
# miss heap-OOB (CLAUDE.md section 7); this one is rebuilt from source every
# time so that cannot happen to it.
fuzz_dyns: src/fuzz/fuzz_dyns.c src/core/dyn-serial.c src/core/dyn-hash.c
	$(CC) -O1 -g -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer \
	  -Isrc/core -o fuzz_dyns $^

# The badssl matrix (design 16). Built from source against the LINKED OpenSSL,
# and only when CONFIG_TLS=y -- without it the file compiles to a stub that
# says so rather than silently passing.
test-tls: tests/test_tls.c src/dyna-tls.c
	$(CC) -O1 -g $(CFLAGS) -Isrc -o $@ $(filter %.c,$^) $(LDFLAGS) $(LIBS)
	@./$@

# The AEAD ciphers, against the specs' own vectors. Needs CONFIG_TLS=y like
# test-tls-conn: the classes are absent otherwise, which the test detects and
# says out loud rather than passing vacuously.
test-crypto-aead: dynajs$(EXE)
	@./dynajs$(EXE) tests/test_crypto_aead.js

# App.static containment: symlink escapes, encoded traversal, with a CANARY
# as the oracle rather than a status code.
test-static-traversal: dynajs$(EXE)
	@./dynajs$(EXE) tests/test_static_traversal.js

# Sanitizer bypass classes NOT covered by test_html.js: mutation XSS,
# namespace confusion, encoded schemes, srcdoc/formaction, base hijack.
test-html-pentest: dynajs$(EXE)
	@./dynajs$(EXE) tests/test_html_pentest.js

# The typed accessor family on Bytes/Text: bounds at the STRADDLE, endianness
# against hand-written bytes, and the transcoders against TextEncoder.
test-bytes-accessors: dynajs$(EXE)
	@./dynajs$(EXE) tests/test_bytes_accessors.js

# The reactor's TLS seam end to end: two engines over a socketpair and a
# payload spanning MANY records, so the one-callback-per-record drain is
# distinguishable from delivering only the first.
# Links the NATIVE MODULE OBJECTS rather than naming dyna-aio.o and its
# dependencies by hand -- that list is exactly what Q31 flags and what broke
# fuzz_net once.
test-aio-tls: tests/test_aio_tls.c dynajs$(EXE)
	$(CC) -O1 -g $(CFLAGS) -Isrc -o $@ tests/test_aio_tls.c \
	  $(NAT_MODULE_OBJS) libdynajs.a $(LDFLAGS) $(LIBS) $(SQLITE_LIBS)
	@./$@

# Server-side TLS, with openssl(1) s_client as the FOREIGN peer: a dynajs
# client against a dynajs server would prove only self-agreement.
test-tls-server: dynajs$(EXE)
	@./dynajs$(EXE) tests/test_tls_server.js

# HTTPClient over TLS: every bad certificate must name its own check, and the
# plaintext control must not move.
test-http-tls: dynajs$(EXE)
	@./dynajs$(EXE) tests/test_http_tls.js

# Crawl BOUNDS against a known link graph, including a deliberate cycle.
test-crawl: dynajs$(EXE)
	@./dynajs$(EXE) tests/test_scrape_crawl.js

# Fetcher POLICY against a mock peer: a real server cannot be made to emit a
# Retry-After or a 9-hop redirect loop on demand.
test-fetcher: dynajs$(EXE)
	@./dynajs$(EXE) tests/test_scrape_fetcher.js

# connect() takes a NAME or an address. The control is a loopback literal:
# it must stay resolver-free.
test-connect-resolve: dynajs$(EXE)
	@./dynajs$(EXE) tests/test_connect_resolve.js

# JWT RS256/ES256, checked by openssl(1) as a FOREIGN oracle: a self round
# trip would pass for an ES token no other library accepts.
test-jwt-asym: dynajs$(EXE)
	@./dynajs$(EXE) tests/test_jwt_asym.js

# Ed25519 / X25519 against RFC 8032 s7.1 and RFC 7748 s6.1.
test-crypto-curve: dynajs$(EXE)
	@./dynajs$(EXE) tests/test_crypto_curve.js

# The TLS SEAM, through TCPServer.connect. Separate from test-tls (which proves
# the engine) because this one needs the whole native build, and separate from
# NATIVE_TESTS because it needs CONFIG_TLS=y, which that build does not set.
test-tls-conn: dynajs$(EXE)
	@./dynajs$(EXE) tests/test_tls_conn.js

# X509/RSA/ECDSA exports are CONFIG_TLS=y only, so this test sits beside
# test-tls-conn instead of in NATIVE_TESTS: the gate's native build has no TLS.
test-x509: dynajs$(EXE)
	@./dynajs$(EXE) tests/test_x509.js

# Same reasoning for the LZ4 decoders: a raw block has no header to reject on,
# so the bounds checks are the entire defence. Rebuilt from source every time.
fuzz_lz4: src/fuzz/fuzz_lz4.c src/core/dyn-compress.c src/core/dyn-hash.c
	$(CC) -O1 -g -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer \
	  -Isrc/core -o fuzz_lz4 $^

# Same source set as test-scram: dyn-codec pulls the SIMD dispatch table, so a
# hand-written list here drifts the moment that dependency moves (Q31).
fuzz_scram: src/fuzz/fuzz_scram.c src/core/dyn-scram.c src/core/dyn-hash.c \
          src/core/dyn-codec.c src/core/dyn-prng.c $(SIMD_SRCS)
	$(CC) -O1 -g -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer \
	  -Isrc/core -Isrc -o fuzz_scram $^

# The three parsers dyna:net points at a peer: RESP, RFC 1035 and SCRAM. Each
# gets its inputs past its own gate -- a tag byte, a repaired header, a started
# exchange -- because a target random bytes bounce off measures the rejection
# and nothing behind it. Rebuilt from source so it cannot pick up objects built
# without the sanitizers.
# The SIMD half comes from $(SIMD_SRCS) rather than a second hand-written copy:
# a duplicated ISA list is what broke fuzz_regexp when lre_exec gained a
# simd_init call, and this rule had the same seven names spelled out again.
# dtoa.c + cutils.c: dyn-resp.c uses js_atod for a correctly-rounded,
# locale-free float parse. A hand-written link line is correct until a
# dependency changes -- this one broke the moment that call was added, which is
# why the gate builds this target.
fuzz_net: src/fuzz/fuzz_net.c src/core/dyn-resp.c src/core/dyn-dns.c \
          src/core/dyn-scram.c src/core/dyn-hash.c src/core/dyn-codec.c \
          src/core/dyn-prng.c src/dtoa.c src/cutils.c $(SIMD_SRCS)
	$(CC) -O1 -g -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer \
	  -Isrc/core -Isrc -o fuzz_net $^

# Every fuzz_* rule belongs here or the gate never links it, and a target whose
# hand-written source list has drifted fails only when somebody finally reaches
# for it. `make fuzz-audit` proves the list matches the rules.
FUZZ_TARGETS = fuzz_eval fuzz_compile fuzz_regexp fuzz_regexp_compile \
               fuzz_json fuzz_bytecode fuzz_module_export fuzz_net \
               fuzz_dyns fuzz_lz4 fuzz_scram fuzz_codec

# Config-gated targets, in ONE place: this was spelled out at five separate
# sites, which is how the sixth gets forgotten and a target silently stops
# being built, linked or smoke-run.
FUZZ_NAT_TARGETS = fuzz_stdlib fuzz_parsers fuzz_dataframe

libfuzzer: $(FUZZ_TARGETS)
ifdef CONFIG_NATIVE_MODULES
libfuzzer: $(FUZZ_NAT_TARGETS)
endif

# FUZZ_NAT_TARGETS is listed separately above because those are config-gated.
# Bidirectional ON PURPOSE. The rules->FUZZ_TARGETS direction alone cannot see
# a SOURCE with no rule, which is how fuzz_regexp_compile.c sat unbuilt and
# unexecuted -- carrying an uninitialised read -- while this audit reported ok.
fuzz-audit:
	@grep -oE '^fuzz_[a-z0-9_]+:' Makefile | tr -d ':' | sort -u > .fz_def.tmp; \
	 printf '%s\n' $(FUZZ_TARGETS) $(FUZZ_NAT_TARGETS) fuzz_csv | sort -u > .fz_gate.tmp; \
	 ls src/fuzz/fuzz_*.c | sed 's|src/fuzz/||; s|\.c$$||' | grep -v '^fuzz_common$$' \
	   | sort -u > .fz_src.tmp; \
	 miss=`comm -23 .fz_def.tmp .fz_gate.tmp`; \
	 orph=`comm -23 .fz_src.tmp .fz_def.tmp`; \
	 n=`wc -l < .fz_def.tmp | tr -d ' '`; \
	 if [ -n "$$miss" ]; then \
	   echo "FAIL: fuzz rules missing from FUZZ_TARGETS (the gate will not link them):"; \
	   echo "$$miss"; exit 1; fi; \
	 if [ -n "$$orph" ]; then \
	   echo "FAIL: fuzz sources with no build rule (never compiled, never run):"; \
	   echo "$$orph"; exit 1; fi; \
	 unbuildable=`comm -13 .fz_def.tmp .fz_gate.tmp`; \
	 if [ -n "$$unbuildable" ]; then \
	   echo "FAIL: gated but unbuildable -- in FUZZ_TARGETS with no rule:"; \
	   echo "$$unbuildable"; exit 1; fi; \
	 rm -f .fz_def.tmp .fz_gate.tmp .fz_src.tmp; \
	 echo "fuzz-audit: all $$n fuzz targets gated"
	@$(MAKE) --no-print-directory tls-link-audit

# Build every target AND prove each carries both sanitizers. A plain `make`
# gives address+undefined via FUZZ_DEFAULT_SAN; CONFIG_ASAN=y is WEAKER because
# an ifndef blanks that variable, so a target built that way has 0 ubsan
# handlers. Measured, not assumed -- check the symbols rather than the flags.
fuzz-all:
	@$(MAKE) --no-print-directory fuzz-audit
	@$(MAKE) --no-print-directory libfuzzer CONFIG_NATIVE_MODULES=y
	@bad=0; for t in $(FUZZ_TARGETS) $(FUZZ_NAT_TARGETS); do \
	   test -x ./$$t || { echo "MISSING: $$t was not built"; bad=1; continue; }; \
	   a=`nm ./$$t 2>/dev/null | grep -c __asan_init`; \
	   u=`nm ./$$t 2>/dev/null | grep -c __ubsan_handle`; \
	   if [ "$$a" -lt 1 ]; then echo "FAIL: $$t has NO AddressSanitizer"; bad=1; \
	   elif [ "$$u" -lt 1 ]; then \
	     echo "FAIL: $$t has NO UndefinedBehaviorSanitizer (built with CONFIG_ASAN=y?)"; bad=1; \
	   else echo "  ok  $$t  asan=$$a ubsan=$$u"; fi; \
	 done; \
	 [ $$bad -eq 0 ] || exit 1; \
	 echo "fuzz-all: every target carries address+undefined"

# A bounded run of each target over its seed corpus. -len_control=0 is NOT a
# tuning preference: libFuzzer grows its length limit on a TIME schedule, so a
# cold short run never approaches -max_len and every execution stays under ~8
# bytes -- 20000 executions of four-character inputs reported as "clean".
# Artifacts go outside the repo: a finding writes a crash-* reproducer into the
# CWD, and `git add -A` would commit it.
FUZZ_SMOKE_RUNS ?= 4000
fuzz-smoke:
	@mkdir -p /tmp/dyna-fuzzart
	@rc=0; for t in $(FUZZ_TARGETS) $(FUZZ_NAT_TARGETS); do \
	   test -x ./$$t || { echo "  SKIP $$t (not built -- run make fuzz-all)"; continue; }; \
	   corp=""; \
	   case $$t in fuzz_stdlib)    corp=src/fuzz/corpus_robots ;; \
	               fuzz_dataframe) corp=src/fuzz/corpus_dataframe ;; \
	               fuzz_dyns)   corp=src/fuzz/corpus_ml ;; esac; \
	   test -n "$$corp" && test -d "$$corp" || corp=""; \
	   printf '  %-22s ' $$t; \
	   ( cd /tmp/dyna-fuzzart && \
	     "$(CURDIR)/$$t" $${corp:+"$(CURDIR)/$$corp"} \
	       -runs=$(FUZZ_SMOKE_RUNS) -len_control=0 -max_len=8192 \
	       -artifact_prefix=/tmp/dyna-fuzzart/ ) > /tmp/dyna-fuzzart/$$t.log 2>&1; \
	   if [ $$? -eq 0 ]; then \
	     echo "ok  $$(grep -o 'cov: [0-9]*' /tmp/dyna-fuzzart/$$t.log | tail -1)$${corp:+  seeded}"; \
	   else \
	     echo "FAIL -- see /tmp/dyna-fuzzart/$$t.log"; rc=1; \
	   fi; \
	 done; \
	 [ $$rc -eq 0 ] && echo "fuzz-smoke: all targets clean at $(FUZZ_SMOKE_RUNS) runs" || exit 1

# test-tls names its two sources by hand (Q31), which is only safe while the
# adapter stays engine-free. Assert that, rather than discovering it the day
# the link breaks for somebody reaching for the badssl matrix.
tls-link-audit:
	@bad=`grep -E '^#include "' src/dyna-tls.c | grep -v 'dyna-tls.h' || true`; \
	 if [ -n "$$bad" ]; then \
	   echo "FAIL: dyna-tls.c gained an engine include, so test-tls's"; \
	   echo "      hand-written source list in the Makefile is now short:"; \
	   echo "$$bad"; exit 1; fi; \
	 echo "tls-link-audit: dyna-tls.c is still engine-free (test-tls links 2 files)"

ifneq ($(CROSS_PREFIX),)

$(DYNAJSC): $(OBJDIR)/dynajsc.host.o \
    $(patsubst %.o, %.host.o, $(DYNAJS_LIB_OBJS))
	$(HOST_CC) $(LDFLAGS) -o $@ $^ $(HOST_LIBS)

endif #CROSS_PREFIX

DYNAJSC_DEFINES:=-DCONFIG_CC=\"$(DYNAJSC_CC)\" -DCONFIG_PREFIX=\"$(PREFIX)\"
ifdef CONFIG_LTO
DYNAJSC_DEFINES+=-DCONFIG_LTO
endif
DYNAJSC_HOST_DEFINES:=-DCONFIG_CC=\"$(HOST_CC)\" -DCONFIG_PREFIX=\"$(PREFIX)\"

$(OBJDIR)/dynajsc.o: CFLAGS+=$(DYNAJSC_DEFINES)
$(OBJDIR)/dynajsc.host.o: CFLAGS+=$(DYNAJSC_HOST_DEFINES)

ifdef CONFIG_LTO
LTOEXT=.lto
else
LTOEXT=
endif

libdynajs$(LTOEXT).a: $(DYNAJS_LIB_OBJS)
	$(AR) rcs $@ $^

ifdef CONFIG_LTO
libdynajs.a: $(patsubst %.o, %.nolto.o, $(DYNAJS_LIB_OBJS))
	$(AR) rcs $@ $^
endif # CONFIG_LTO

libdynajs.fuzz.a: $(patsubst %.o, %.fuzz.o, $(DYNAJS_LIB_OBJS))
	$(AR) rcs $@ $^

repl.c: $(DYNAJSC) repl.js
	$(DYNAJSC) -s -c -o $@ -m repl.js

ifneq ($(wildcard unicode/UnicodeData.txt),)
$(OBJDIR)/libunicode.o $(OBJDIR)/libunicode.nolto.o: libunicode-table.h

libunicode-table.h: unicode_gen
	./unicode_gen unicode $@
endif

run-test262$(EXE): $(OBJDIR)/run-test262.o $(DYNAJS_LIB_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

run-test262-debug: $(patsubst %.o, %.debug.o, $(OBJDIR)/run-test262.o $(DYNAJS_LIB_OBJS))
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

# object suffix order: nolto

$(OBJDIR)/%.o: %.c | $(OBJDIR)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS_OPT) -c -o $@ $<

# mimalloc v3 single-file amalgamation (built release, independent of engine CFLAGS)
$(OBJDIR)/mimalloc.o: third_party/mimalloc/src/static.c | $(OBJDIR)
	@mkdir -p $(@D)
	$(CC) -O2 -DNDEBUG -Ithird_party/mimalloc/include -c -o $@ $<

$(OBJDIR)/fuzz_%.o: src/fuzz/fuzz_%.c | $(OBJDIR)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS_OPT) -c -I. -o $@ $<

$(OBJDIR)/%.host.o: %.c | $(OBJDIR)
	@mkdir -p $(@D)
	$(HOST_CC) $(CFLAGS_OPT) -c -o $@ $<

$(OBJDIR)/%.pic.o: %.c | $(OBJDIR)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS_OPT) -fPIC -DJS_SHARED_LIBRARY -c -o $@ $<

$(OBJDIR)/%.nolto.o: %.c | $(OBJDIR)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS_NOLTO) -c -o $@ $<

$(OBJDIR)/%.debug.o: %.c | $(OBJDIR)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS_DEBUG) -c -o $@ $<

$(OBJDIR)/%.fuzz.o: %.c | $(OBJDIR)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS_OPT) $(FUZZ_DEFAULT_SAN) -fsanitize=fuzzer-no-link -c -o $@ $<

$(OBJDIR)/%.check.o: %.c | $(OBJDIR)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -DCONFIG_CHECK_JSVALUE -c -o $@ $<

unicode_gen: $(OBJDIR)/unicode_gen.host.o $(OBJDIR)/cutils.host.o libunicode.c unicode_gen_def.h
	$(HOST_CC) $(LDFLAGS) $(CFLAGS) -o $@ $(OBJDIR)/unicode_gen.host.o $(OBJDIR)/cutils.host.o

clean:
	rm -f repl.c
	rm -f *.a *.o *.d *~ unicode_gen $(FUZZ_TARGETS) $(FUZZ_NAT_TARGETS) fuzz_csv $(PROGS)
	rm -f hello.c test_fib.c
	rm -f examples/*.so tests/*.so
	rm -rf $(OBJDIR)/ *.dSYM/ dyna-debug$(EXE)
	rm -rf run-test262-debug$(EXE)

install: all
	mkdir -p "$(DESTDIR)$(PREFIX)/bin"
	$(STRIP) dynajs$(EXE) dynajsc$(EXE)
	install -m755 dynajs$(EXE) dynajsc$(EXE) "$(DESTDIR)$(PREFIX)/bin"
	mkdir -p "$(DESTDIR)$(PREFIX)/lib/dynajs"
	install -m644 libdynajs.a "$(DESTDIR)$(PREFIX)/lib/dynajs"
ifdef CONFIG_LTO
	install -m644 libdynajs.lto.a "$(DESTDIR)$(PREFIX)/lib/dynajs"
endif
	mkdir -p "$(DESTDIR)$(PREFIX)/include/dynajs"
	install -m644 src/dynajs.h src/dyna-libc.h "$(DESTDIR)$(PREFIX)/include/dynajs"

###############################################################################
# examples

# example of static JS compilation
HELLO_SRCS=examples/hello.js
HELLO_OPTS=-fno-string-normalize -fno-map -fno-promise -fno-typedarray \
           -fno-typedarray -fno-regexp -fno-json -fno-eval -fno-proxy \
           -fno-date -fno-module-loader

hello.c: $(DYNAJSC) $(HELLO_SRCS)
	$(DYNAJSC) -e $(HELLO_OPTS) -o $@ $(HELLO_SRCS)

examples/hello: $(OBJDIR)/hello.o $(DYNAJS_LIB_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

# example of static JS compilation with modules
HELLO_MODULE_SRCS=examples/hello_module.js
HELLO_MODULE_OPTS=-fno-string-normalize -fno-map -fno-typedarray \
           -fno-typedarray -fno-regexp -fno-json -fno-eval -fno-proxy \
           -fno-date -m
examples/hello_module: $(DYNAJSC) libdynajs$(LTOEXT).a $(HELLO_MODULE_SRCS)
	$(DYNAJSC) $(HELLO_MODULE_OPTS) -o $@ $(HELLO_MODULE_SRCS)

# use of an external C module (static compilation)

test_fib.c: $(DYNAJSC) examples/test_fib.js
	$(DYNAJSC) -e -M examples/fib.so,fib -m -o $@ examples/test_fib.js

examples/test_fib: $(OBJDIR)/test_fib.o $(OBJDIR)/examples/fib.o libdynajs$(LTOEXT).a
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

examples/fib.so: $(OBJDIR)/examples/fib.pic.o
	$(CC) $(LDFLAGS) -shared -o $@ $^

examples/point.so: $(OBJDIR)/examples/point.pic.o
	$(CC) $(LDFLAGS) -shared -o $@ $^

###############################################################################
# tests

ifdef CONFIG_SHARED_LIBS
test: tests/bjson.so examples/point.so
endif

CORE_TESTS = tests/test_closure.js tests/test_language.js tests/test_modern.js \
  tests/test_disposable.js tests/test_array_ext.js tests/test_iterator_lazy.js \
  tests/test_array_sorted.js tests/test_typedarray_ext.js tests/test_string_fill.js \
  tests/test_date_json_fmt.js tests/test_array_dense_get.js tests/test_array_search.js \
  tests/test_string_ext.js tests/test_string_ext2.js tests/test_string_ansi.js \
  tests/test_ext_batch7.js tests/test_ext_batch8.js tests/oracle_parse_string.js \
  tests/oracle_parse_ident.js tests/oracle_line_col.js tests/oracle_expr_precedence.js \
  tests/oracle_asi.js tests/test_date_ext.js tests/test_lens_ext.js \
  tests/test_number_ext.js tests/test_number_ext2.js tests/test_object_ext.js \
  tests/test_object_ext2.js tests/test_function_ext.js tests/test_fn_timers.js \
  tests/test_optimizer.js tests/test_loop.js tests/test_bigint.js \
  tests/test_bigint_asuintn.js tests/probe_audit_leads.js tests/test_textcodec.js \
  tests/test_string_hash.js tests/test_cyclic_import.js tests/test_worker.js

test: dynajs$(EXE)
	$(WINE) ./dynajs$(EXE) --std tests/test_builtin.js
	@./tools/run-tests-parallel.sh $(CORE_TESTS) || exit 1
# dynajsc writes function bytecode into generated C; running the result reads it
# back. These programs were BUILT by `make all` and never RUN, so that whole
# write->read path had no coverage at all.
#
# It is NOT a format-drift detector, and that was measured rather than assumed:
# injecting an extra flag bit -- both symmetrically and writer-only -- left both
# programs printing "Hello World". Writer and reader come from one build so a
# coordinated change is invisible by construction, and these programs are too
# trivial for a misread flag to change what they do. Detecting drift needs two
# builds, which is what BC_VERSION is for.
	@$(MAKE) --no-print-directory examples/hello examples/hello_module examples/test_fib
	$(WINE) ./examples/hello
	$(WINE) ./examples/hello_module
	$(WINE) ./examples/test_fib
ifndef CONFIG_WIN32
	$(WINE) ./dynajs$(EXE) tests/test_std.js
	$(WINE) ./dynajs$(EXE) tests/test_rw_handler.js
	$(WINE) ./dynajs$(EXE) tests/test_async_api.js
	$(WINE) ./dynajs$(EXE) tests/test_async_leak.js
endif
ifdef CONFIG_SHARED_LIBS
	$(WINE) ./dynajs$(EXE) tests/test_bjson.js
	$(WINE) ./dynajs$(EXE) examples/test_point.js
endif
	@$(MAKE) --no-print-directory check-hooks

# Native-module (dyna:*) tests. These import modules that only exist in a
# CONFIG_NATIVE_MODULES=y build, so `make test` cannot run them:
#   make clean && make CONFIG_NATIVE_MODULES=y && make test-native
# Excluded on purpose: the http/uring tests (bind ports / need Linux io_uring).
# They are NOT unrun -- the uring-tests stage runs all of them on Linux
# against BOTH backends (epoll and io_uring):
#   docker build --target uring-tests -f docker/Dockerfile -t dynascript:uringtests .
#   docker run --rm --security-opt seccomp=unconfined dynascript:uringtests
# seccomp=unconfined is required; the default profile blocks io_uring_setup.
# Add every new dyna:* test file here or nothing runs it.
NATIVE_TESTS=tests/test_algo_blackbox.js tests/test_http_security.js tests/test_http_pentest.js tests/test_http_upload.js tests/test_structures_gaps.js tests/test_bytes.js tests/test_bytes_handle.js tests/test_compress.js \
  tests/test_capabilities.js tests/test_sys_memory.js tests/test_lz4.js tests/test_dictionary.js tests/test_mathx_tierb.js tests/oracle_compress_bytes.js tests/test_iterator_gap.js tests/test_mathx_matlab.js tests/test_ml_metrics.js tests/test_ml_weights.js tests/test_ml_pipeline.js tests/test_ml_selection.js tests/test_ml_sklearn.js \
  tests/test_structures_iter.js tests/test_structures_guava.js \
  tests/test_structures_serialize.js tests/test_structures_serde.js \
  tests/test_structures_bitset_codec.js tests/test_structures_sorted_codec.js \
  tests/test_structures_hll.js tests/test_structures_trie_codec.js \
  tests/test_structures_numeric_codec.js tests/test_structures_heap_natural.js \
  tests/test_structures_multiset_codec.js tests/test_structures_table_slice.js \
  tests/test_structures_btree.js tests/test_structures_trie_paths.js \
  tests/test_structures_record_codecs.js tests/test_structures_itree_pending.js \
  tests/test_structures_adversarial.js tests/test_structures_codec_forge.js \
  tests/test_structures_graph.js \
  tests/test_net_tcp.js tests/test_net_dns.js tests/test_net_sqlite.js \
  tests/test_net_pentest.js tests/test_proxy.js tests/test_http_proxy.js tests/test_watch.js tests/test_html_text.js tests/test_xml_text.js tests/test_yaml_scalar.js tests/test_scrape_robots.js tests/test_crypto_otp.js tests/test_crypto_jwt.js tests/test_scrape_extract.js \
   tests/test_net_redis.js tests/test_net_pg.js tests/test_net_pg_stmt.js \
   tests/test_net_pg_binary.js \
   tests/test_net_e2e.js \
   tests/test_net_fragment.js \
   tests/test_net_rss.js \
   tests/test_net_eyeballs.js \
   tests/probe_audit_leads2.js \
  tests/test_crypto.js tests/test_crypto_reuse.js tests/test_hash_split.js tests/test_csv.js tests/test_dataframe.js \
  tests/test_optguide_regressions.js \
  tests/test_http_async_bounds.js \
  tests/test_http_client_async.js \
  tests/test_http_sse.js \
  tests/test_http_compress.js \
  tests/test_http_metrics_endpoint.js \
  tests/test_http_hardening.js \
  tests/test_ws_client.js \
  tests/test_encoding.js \
  tests/test_file.js tests/test_file_handle.js tests/test_file_async.js \
  tests/test_file_platform.js tests/test_file_lock.js \
  tests/test_matcher.js tests/test_approx_match.js tests/test_diff.js \
  tests/test_ids.js tests/test_config.js tests/test_json5.js tests/test_iconv.js \
  tests/test_jsonpath.js tests/test_xml.js \
  tests/test_proc.js tests/test_yaml.js tests/test_decimal.js tests/test_vserialize.js tests/test_archive.js tests/test_archive_pax.js tests/test_html.js tests/test_markdown.js tests/test_basex.js tests/test_expr.js tests/test_sha3.js tests/test_template.js tests/test_blake.js tests/test_sys_machine.js tests/test_lru_ttl.js tests/test_ratelimit.js tests/test_metrics.js tests/test_file_copy.js tests/test_enum_order.js tests/test_qr.js tests/test_temporal.js \
  tests/test_log.js tests/test_url.js tests/test_httpmsg.js tests/test_cli.js \
  tests/test_validate.js tests/test_validate_ext.js tests/test_json.js \
  tests/test_protobuf.js tests/test_asn1.js tests/test_codec_zstd.js \
  tests/test_multipart.js tests/test_idna.js tests/test_tls_roots.js tests/test_toml.js \
  tests/test_random_inputs.js \
  tests/test_mathx.js tests/test_ml.js tests/test_ml_preprocessing.js \
  tests/test_ml_neighbors.js tests/test_ml_decomposition.js \
  tests/test_ml_trees.js tests/test_ml_svm.js tests/test_ml_persist.js \
  tests/test_ml_tree_proba.js tests/test_ml_boosting.js tests/test_ml_production.js \
  tests/oracle_ml_hist.js tests/test_ml_xgb.js tests/test_ml_sparse.js tests/test_time_format.js tests/test_time_dateparser.js \
  tests/test_netip.js tests/test_random.js \
  tests/test_semver.js \
  tests/test_simd.js tests/test_simd_f64.js tests/test_simd_int.js \
  tests/test_structures.js \
  tests/test_sys.js tests/test_text_kernels.js tests/test_text_simd.js \
  tests/test_time.js tests/test_rrule.js tests/test_utf16.js tests/test_uuid.js \
  tests/test_schema.js tests/test_crypto_standalone.js \
  tests/test_fetch.js \
  tests/test_fetch_body.js tests/test_module_interop.js \
  tests/test_rpc_params.js tests/test_keepalive.js tests/test_url_host.js

# NB: deliberately NOT dependent on dynajs$(EXE). Making it a prerequisite lets a
# bare `make test-native` RELINK the binary with the default flags -- which drops
# NAT_MODULE_OBJS while leaving the .o files untouched, producing a dynajs that
# cannot load any dyna:* module. Check the built binary instead and say so.
# Pure-C differential test of src/core/dyn-ds.c against brute-force models. It
# does not link the engine, so it builds and runs standalone under
# ASan+UBSan -- which is where an open-addressing table's bugs actually show.
# Symlink-safety of the atomic file write. Standalone so it runs under
# ASan+UBSan without linking the engine.
test-io-atomic:
	$(CC) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
	  -Wall -Wextra -std=gnu17 -Isrc \
	  tests/test_io_atomic_write.c src/dyna-io.c src/cutils.c -o .obj/test_io_atomic
	.obj/test_io_atomic

# The perf/correctness harnesses written for a specific subsystem. Each exists
# because an existing benchmark could not see a real defect.
#
# EVERY ROW FAILS THE TARGET. An earlier version wrote `2>/dev/null || true` and
# `-` on four of the five rows, under a header claiming they needed
# CONFIG_NATIVE_MODULES=y. Not one of these files references a dyna:* module, so
# the header was false and every `-` was unconditional: `make bench-core` could
# not fail. That is the exact defect this header was written to name -- a suite
# that cannot fail reads as coverage -- reintroduced by the fix for it.
# Verified by deleting a bench file: the old form exited 0, this one
# exits 2.
#
# The `2>/dev/null` on the corpus row was separately wrong. That row does not
# need it to run; what it suppressed was the corpus builder's own report, which
# goes to stderr and carries the SKIP COUNT -- the one number CLAUDE.md insists
# a corpus print, because a builder that silently drops its largest files while
# reporting a size is flattering itself.
#
# The framework corpus is the one legitimate skip: ~31 MB fetched over the
# network into a gitignored directory, so it is absent on a fresh clone. It is
# announced and names the script, never silently passed over.
# The start-position prefilter against a brute-force reference. Standalone, so
# it runs under ASan+UBSan without linking the engine -- and it covers what the
# random fuzzer provably cannot (see the file header).
test-regexp-prefilter:
	$(CC) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
	  -Wall -Wextra -Wno-unused-parameter -std=gnu17 -I. -Isrc \
	  tests/test_regexp_prefilter.c src/libregexp.c src/cutils.c src/libunicode.c \
	  $(SIMD_SRCS) -o .obj/test_regexp_prefilter
	ASAN_OPTIONS=detect_leaks=0 .obj/test_regexp_prefilter

# js_atod's subnormal rounding against libc strtod (correctly rounded). Pins a
# property that the limb-width differential and the dtoa round-trip both miss.
test-dtoa-subnormal:
	$(CC) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
	  -Wall -Wextra -Wno-unused-parameter -std=gnu17 -I. -Isrc \
	  tests/test_dtoa_subnormal.c src/dtoa.c src/cutils.c -o .obj/test_dtoa_subnormal
	ASAN_OPTIONS=detect_leaks=0 .obj/test_dtoa_subnormal

# simd.find_bitmap against the portable reference, for whichever ISA the
# dispatch table installed. Run it under the amd64 stage too: the x86
# kernels never execute on an arm64 host.
test-simd-bitmap:
	$(CC) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
	  -Wall -Wextra -Wno-unused-parameter -std=gnu17 -I. -Isrc \
	  tests/test_simd_bitmap.c $(SIMD_SRCS) src/cutils.c -o .obj/test_simd_bitmap
	ASAN_OPTIONS=detect_leaks=0 .obj/test_simd_bitmap

# Every src/core TU: a timing side (8 bytes to 1 MB, so a kernel that wins big
# and loses small is visible) and a differential side that hashes every
# observable. A/B any core change by diffing the hash and the times together --
# the fastest way to compute something is to compute it wrongly.
#
# This list is the coverage claim, so keep it EQUAL to src/core/*.c. It read
# "every src/core TU" for a while with mathx, dict and compress missing -- 51%
# of the directory by line count -- which meant a change to any of them could
# not be measured at all, and the comment said otherwise.
CORE_BENCH_SRCS=src/core/dyn-ds.c src/core/dyn-codec.c src/core/dyn-path.c \
  src/core/dyn-prng.c src/core/dyn-ac.c src/core/dyn-serial.c src/core/dyn-hash.c \
  src/core/dyn-mathx.c src/core/dyn-dict.c src/core/dyn-compress.c
bench-core-tus:
	$(CC) -O2 -std=gnu17 -I src/core -I src -Wall -Wextra -Wno-unused-parameter \
	  tests/bench_core.c $(CORE_BENCH_SRCS) $(SIMD_SRCS) src/cutils.c -lm \
	  -o .obj/bench_core
	.obj/bench_core

bench-core:
	./dynajs$(EXE) --std tests/bench_parse_corpus.js
	./dynajs$(EXE) tests/bench_regexp.js
	./dynajs$(EXE) tests/bench_numeric.js
	./dynajs$(EXE) --std tests/bench_stdio.js
	@if ./dynajs$(EXE) -e 'import("dyna:sys")' >/dev/null 2>&1; then \
	  ./dynajs$(EXE) --std tests/bench_regexp_memory.js; \
	else \
	  echo "=== SKIP bench_regexp_memory: needs CONFIG_NATIVE_MODULES=y for dyna:sys"; \
	fi
	@if ls bench/frameworks/*.js >/dev/null 2>&1; then \
	  ./dynajs$(EXE) --std tests/bench_parse_frameworks.js; \
	else \
	  echo "=== SKIP bench_parse_frameworks: no corpus in bench/frameworks/"; \
	  echo "===      fetch it with tests/fetch_frameworks.sh"; \
	fi

# Regex differential oracle. The RESULT HASH is the oracle, not the identity
# checks: build twice (once with -DCONFIG_REGEXP_BACKREF_MEMCMP=0) and diff.
oracle-dtoa:
	./dynajs$(EXE) tests/oracle_dtoa.js

oracle-regexp:
	./dynajs$(EXE) tests/oracle_regexp_fuzz.js 100000

test-ds-core:
	$(CC) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
	  -Wall -Wextra -std=gnu17 -Isrc/core \
	  tests/test_ds_core.c src/core/dyn-ds.c src/core/dyn-hash.c -o .obj/test_ds_core
	.obj/test_ds_core

# The work pool. Built TWICE: a clean single-threaded pass says nothing about a
# race, so the TSan build is the one that carries the concurrency claim.
test-pool:
	@mkdir -p .obj
	$(CC) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
	  -Wall -Wextra -std=gnu17 -Isrc/core \
	  tests/test_pool.c src/core/dyn-pool.c -lpthread -o .obj/test_pool
	.obj/test_pool
	$(CC) -g -O1 -fsanitize=thread -fno-omit-frame-pointer \
	  -Wall -Wextra -std=gnu17 -Isrc/core \
	  tests/test_pool.c src/core/dyn-pool.c -lpthread -o .obj/test_pool_tsan
	.obj/test_pool_tsan

# Descriptor hygiene. The LOW LIMIT is the test: a leak of one fd per cycle is
# invisible at a normal ulimit and fatal at 64, so the target sets it. Needs a
# native build; skips loudly rather than passing silently without one.
test-net-fdchurn:
	@./dynajs$(EXE) -e 'import("dyna:net")' >/dev/null 2>&1 || { \
	  echo "SKIP: test-net-fdchurn needs CONFIG_NATIVE_MODULES=y"; exit 1; }
	@( ulimit -n 64; ./dynajs$(EXE) tests/test_net_fdchurn.js ) | tee /dev/stderr | \
	  grep -q "errors=0" || { echo "FAIL: descriptors leak across the cycle"; exit 1; }

# Happy Eyeballs race hygiene. Same low-ulimit shape as test-net-fdchurn: a
# loser descriptor leaked per connectHappy exhausts 64 fds inside the churn
# and every later race then reports a connect failure.
test-net-eyeballs-churn:
	@./dynajs$(EXE) -e 'import("dyna:net")' >/dev/null 2>&1 || { \
	  echo "SKIP: test-net-eyeballs-churn needs CONFIG_NATIVE_MODULES=y"; exit 1; }
	@( ulimit -n 64; ./dynajs$(EXE) tests/test_net_eyeballs.js 120 ) | tee /dev/stderr | \
	  grep -q "errors=0" || { echo "FAIL: the race loser leaks a descriptor"; exit 1; }

# Timers. Time is injected, not read from the clock, so this asserts ORDER and
# COUNT rather than durations -- a duration assert fails for reasons that are
# not bugs.
# The RFC 1035 codec, driven adversarially. Pure C, no sockets: the parser that
# reads attacker bytes is testable and fuzzable on its own.
test-dns-codec:
	@mkdir -p .obj
	$(CC) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
	  -Wall -Wextra -std=gnu17 -Isrc/core \
	  tests/test_dns_codec.c src/core/dyn-dns.c -o .obj/test_dns_codec
	.obj/test_dns_codec

# The RESP codec, driven adversarially. Same shape as test-dns-codec: pure C,
# so the parser that reads a peer's bytes needs no socket to be attacked.
test-resp-codec:
	@mkdir -p .obj
	@# -Isrc and dtoa.c: dyn-resp.c uses js_atod for a correctly-rounded,
	@# locale-free float parse. A standalone target inherits NONE of the main
	@# build's include paths or objects, so it must name them itself.
	$(CC) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
	  -Wall -Wextra -std=gnu17 -Isrc/core -Isrc \
	  tests/test_resp_codec.c src/core/dyn-resp.c src/dtoa.c src/cutils.c \
	  -o .obj/test_resp_codec -lm
	.obj/test_resp_codec

# SCRAM-SHA-256, client side. Two oracles: the RFC 7677 vector computed from
# the definition, and a server that verifies the proof the way a real one does.
test-scram:
	@mkdir -p .obj
	$(CC) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
	  -Wall -Wextra -std=gnu17 -Isrc/core \
	  tests/test_scram.c src/core/dyn-scram.c src/core/dyn-hash.c \
	  src/core/dyn-codec.c src/core/dyn-prng.c src/core/dyn-simd-stub.c \
	  -o .obj/test_scram 2>/dev/null || \
	$(CC) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
	  -Wall -Wextra -std=gnu17 -Isrc/core -Isrc \
	  tests/test_scram.c src/core/dyn-scram.c src/core/dyn-hash.c \
	  src/core/dyn-codec.c src/core/dyn-prng.c $(SIMD_SRCS) \
	  -o .obj/test_scram
	.obj/test_scram

test-timer:
	@mkdir -p .obj
	$(CC) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
	  -Wall -Wextra -std=gnu17 -Isrc/core \
	  tests/test_timer.c src/core/dyn-timer.c -o .obj/test_timer
	.obj/test_timer

# The disk entry points, which are serviced by the pool on this backend. TSan
# as well: the work runs on a worker and the callback on the loop thread.
AIO_DISK_SRCS=tests/test_aio_disk.c src/dyna-aio.c src/dyna-evloop.c \
              src/dyna-io.c src/core/dyn-pool.c src/cutils.c
test-aio-disk:
	@mkdir -p .obj
	$(CC) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
	  -std=gnu17 -D_GNU_SOURCE -DCONFIG_NATIVE_MODULES -Isrc -Isrc/core \
	  $(AIO_DISK_SRCS) -lpthread -o .obj/test_aio_disk
	.obj/test_aio_disk
	$(CC) -g -O1 -fsanitize=thread -fno-omit-frame-pointer \
	  -std=gnu17 -D_GNU_SOURCE -DCONFIG_NATIVE_MODULES -Isrc -Isrc/core \
	  $(AIO_DISK_SRCS) -lpthread -o .obj/test_aio_disk_tsan
	.obj/test_aio_disk_tsan

# The hardware CRC32C path against the bit-serial definition. Built at -O2
# because the ISA path is only selected in an optimised build, and with the
# SIMD TUs because dyn-hash calls simd.* directly.
test-crc32c-hw:
	$(CC) -O2 -std=gnu17 -Isrc/core -Isrc -Wall -Wextra \
	  tests/test_crc32c_hw.c src/core/dyn-hash.c $(SIMD_SRCS) -lm \
	  -o .obj/test_crc32c_hw
	.obj/test_crc32c_hw

# dyn_crc32c from 8 threads at once. A sanitizer only reports races it
# actually executes, so a clean TSan run over the single-threaded suite proves
# nothing about a function reachable from every worker thread.
test-crc32c-race:
	$(CC) -O1 -g -std=gnu17 -fsanitize=thread -fno-omit-frame-pointer \
	  -Isrc/core -Isrc -Wall -Wextra \
	  tests/test_crc32c_race.c src/core/dyn-hash.c $(SIMD_SRCS) -lm \
	  -o .obj/test_crc32c_race
	.obj/test_crc32c_race

# Hardware SHA-256 against the published vectors and against the scalar path.
# Both configurations are built, because "they agree" is only meaningful if the
# control is the implementation the fast path replaced.
test-sha256-hw:
	$(CC) -O2 -std=gnu17 -Isrc/core -Isrc -Wall -Wextra \
	  tests/test_sha256_hw.c src/core/dyn-hash.c $(SIMD_SRCS) -lm \
	  -o .obj/test_sha256_hw
	$(CC) -O2 -std=gnu17 -Isrc/core -Isrc -Wall -Wextra -DDYN_SHA256_NO_HW=1 \
	  tests/test_sha256_hw.c src/core/dyn-hash.c $(SIMD_SRCS) -lm \
	  -o .obj/test_sha256_sw
	@echo "  hardware:"; .obj/test_sha256_hw
	@echo "  scalar:  "; .obj/test_sha256_sw

# The REPL's line editor, history and completion only run against a terminal,
# so this drives a real pty from python3 rather than piping stdin. A skip is
# printed rather than silent: a lower case count with no failures reads green.
# Adversarial suites, kept OUT of `make test` on purpose: they spawn processes
# and sockets, so a slow or flaky case here must not be mistaken for a hung
# default build.
SECURITY_TESTS = tests/test_http_params.js \
                 tests/test_parser_pentest.js \
                 tests/test_module_pentest.js \
                 tests/test_ext_pentest.js \
                 tests/test_netfile_pentest.js \
                 tests/test_html_pentest.js \
                 tests/test_static_traversal.js \
                 tests/test_net_pentest.js \
                 tests/test_http_pentest.js \
                 tests/test_data_pentest.js \
                 bench/_sec.js

# Same no-prerequisite rule as test-repl: rebuilding here would silently swap a
# CONFIG_NATIVE_MODULES=y binary for a default one and the suites would still pass.
test-security:
	@test -x ./dynajs$(EXE) || { \
	  echo "FAIL: no ./dynajs$(EXE). Build one first: make CONFIG_NATIVE_MODULES=y"; exit 1; }
	@./dynajs$(EXE) -e 'import("dyna:mathx")' >/dev/null 2>&1 || { \
	  echo "FAIL: ./dynajs cannot load dyna:* modules."; \
	  echo "      Build one first: make clean && make CONFIG_NATIVE_MODULES=y"; \
	  exit 1; }
	@./tools/run-tests-parallel.sh $(SECURITY_TESTS) || exit 1
	@echo "test-security: all suites passed"

# The five layers over the dyna:* API, weakest oracle first. Each answers a
# question the one before it cannot: a surface sweep proves a name is TOTAL, a
# differential proves it agrees with an obvious implementation, a round trip
# proves a pair is consistent, and only a published vector proves the digits.
# Run in this order so a total-function failure is reported before a value one.
API_TESTS = tests/test_api_surface.js \
            tests/test_api_params.js \
            tests/test_api_differential.js \
            tests/test_api_roundtrip.js \
            tests/test_api_vectors.js \
            tests/test_api_kernels.js \
            tests/test_api_properties.js \
            tests/test_api_fuzz.js

# Same no-prerequisite probe as test-native: a rebuild here would silently
# produce a default-config binary in which every dyna:* case SKIPS, and a run
# of all-skips prints no failures.
test-api:
	@test -x ./dynajs$(EXE) || { \
	  echo "FAIL: no ./dynajs$(EXE). Build one first: make CONFIG_NATIVE_MODULES=y"; exit 1; }
	@./dynajs$(EXE) -e 'import("dyna:mathx")' >/dev/null 2>&1 || { \
	  echo "FAIL: ./dynajs cannot load dyna:* modules."; \
	  echo "      Build one first: make clean && make CONFIG_NATIVE_MODULES=y"; \
	  exit 1; }
	@for t in $(API_TESTS); do \
	  test -f $$t || { echo "FAIL: $$t is listed but missing"; exit 1; }; \
	  echo "=== $$t"; \
	  $(WINE) ./dynajs$(EXE) $$t || { echo "FAIL: $$t"; exit 1; }; \
	done
	@echo "test-api: all API suites passed"

# The API surface this regime is supposed to cover, enumerated from the BINARY.
# Native members are non-enumerable, so a class-granularity or Object.keys sweep
# reports far fewer names than exist.
api-inventory:
	@test -x ./dynajs$(EXE) || { echo "FAIL: build ./dynajs first"; exit 1; }
	@./dynajs$(EXE) tools/api-inventory.js

# No dynajs prerequisite ON PURPOSE: it would rebuild in the DEFAULT config and
# silently replace a CONFIG_NATIVE_MODULES=y binary, and every case would still
# pass because none of them touch dyna:*. Probe instead, like test-native.
test-repl:
	@test -x ./dynajs$(EXE) || { \
	  echo "FAIL: no ./dynajs$(EXE). Build one first: make"; exit 1; }
	@command -v python3 >/dev/null 2>&1 || { \
	  echo "=== SKIP test-repl: python3 not found, the pty harness needs it"; \
	  exit 0; }
	@python3 tests/test_repl.py --binary ./dynajs$(EXE)

# ---------------------------------------------------------------- pre-push
# THE MANDATORY GATE. Everything that must be true before anything leaves this
# machine, in one target, so there is exactly one thing to run and one thing to
# install as a hook.
#
# It BUILDS ITS OWN BINARY rather than testing whatever is lying around. That is
# not caution, it is the difference between a gate and a decoration: half these
# suites SKIP their dyna:* sections against a default-config binary and a run of
# all-skips prints zero failures. `clean` first because make tracks timestamps,
# not flags, so a tree last built without CONFIG_NATIVE_MODULES=y would relink
# the old objects and the probe below would still pass.
#
# Ordered cheapest-first so a broken build or a stale doc reference fails in
# seconds rather than after the fuzz suites.
# The TLS stack was compiled by NO gate: dev.sh sets CONFIG_TLS nowhere and the
# native build does not either, so dyna-tls.c, HTTPS and the AEAD ciphers were
# never built, let alone run, while asan/ubsan reported ok. Detect OpenSSL the
# same way the CONFIG_TLS block does and gate WITH it when present.
PREPUSH_OPENSSL_PC:=$(shell brew --prefix openssl@3 2>/dev/null || brew --prefix openssl 2>/dev/null)/lib/pkgconfig
PREPUSH_TLS:=$(shell PKG_CONFIG_PATH="$(PREPUSH_OPENSSL_PC):$$PKG_CONFIG_PATH" \
  pkg-config --exists 'openssl >= 3.0' 2>/dev/null && echo y)
PREPUSH_J:=$(shell (command -v nproc >/dev/null 2>&1 && nproc) || sysctl -n hw.ncpu 2>/dev/null || echo 4)

prepush:
	@t0=$$(date +%s); \
	 echo "================================================================="; \
	 echo "prepush: starting 10-stage proof gate (parallel build: -j$(PREPUSH_J), TLS: $(if $(PREPUSH_TLS),yes,no))"; \
	 echo "================================================================="; \
	 echo "[1/10] codegraph (source shape; parses source, no binary)..."; \
	 s0=$$(date +%s); \
	 python3 bench/codegraph.py . --report > bench/codegraph_report.txt 2>&1 || { \
	   echo "FAIL: codegraph exited non-zero."; tail -20 bench/codegraph_report.txt; exit 1; }; \
	 sum=`grep -E '^codegraph-[a-z0-9]+: [0-9]+ files parsed' bench/codegraph_report.txt | head -1`; \
	 nf=`printf '%s' "$$sum" | sed -E 's/^codegraph-[a-z0-9]+: ([0-9]+) files.*/\1/'`; \
	 nfn=`grep -E '^ *[0-9]+ symbols parsed' bench/codegraph_report.txt | head -1 | sed -E 's/ *([0-9]+) symbols.*/\1/'`; \
	 case "$$nf$$nfn" in ''|*[!0-9]*) nf=0; nfn=0;; esac; \
	 if [ "$$nf" -lt 1 ] || [ "$$nfn" -lt 1 ]; then \
	   echo "FAIL: codegraph parsed $$nf files / $$nfn symbols -- wrong root or moved tree."; \
	   exit 1; fi; \
	 s1=$$(date +%s); \
	 echo "       $$sum ($$nfn symbols) in $$((s1 - s0))s -- OK"; \
	 echo "[2/10] clean build (-j$(PREPUSH_J), CONFIG_NATIVE_MODULES=y$(if $(PREPUSH_TLS), CONFIG_TLS=y))..."; \
	 s0=$$(date +%s); \
	 $(MAKE) --no-print-directory clean >/dev/null && \
	 $(MAKE) -j$(PREPUSH_J) --no-print-directory CONFIG_NATIVE_MODULES=y $(if $(PREPUSH_TLS),CONFIG_TLS=y) || { \
	   echo "FAIL: build failed"; exit 1; }; \
	 ./dynajs$(EXE) -e 'import("dyna:mathx")' >/dev/null 2>&1 || { \
	   echo "FAIL: prepush built a binary that cannot load dyna:*. The suites"; \
	   echo "      would have SKIPPED their dyna:* sections and reported green."; \
	   exit 1; }; \
	 s1=$$(date +%s); \
	 echo "       build finished in $$((s1 - s0))s -- OK"; \
	 echo "[3/10] fuzz-audit + libfuzzer link proof..."; \
	 s0=$$(date +%s); \
	 $(MAKE) --no-print-directory fuzz-audit && \
	 $(MAKE) -j$(PREPUSH_J) --no-print-directory libfuzzer > $(OBJDIR)/fuzzlink.log 2>&1 || { \
	   echo "FAIL: a fuzz target no longer links -- a hand-written rule lost an object."; \
	   tail -25 $(OBJDIR)/fuzzlink.log; exit 1; }; \
	 s1=$$(date +%s); \
	 echo "       fuzz targets verified in $$((s1 - s0))s -- OK"; \
	 echo "[4/10] check-imports & orphan test audit..."; \
	 s0=$$(date +%s); \
	 python3 tools/check-unused-imports.py tests && \
	 python3 tools/check-orphan-tests.py || { echo "FAIL: import or orphan audit failed"; exit 1; }; \
	 s1=$$(date +%s); \
	 echo "       imports & tests clean in $$((s1 - s0))s -- OK"; \
	 echo "[5/10] core test suite (make test)..."; \
	 s0=$$(date +%s); \
	 $(MAKE) --no-print-directory test || { echo "FAIL: core tests failed"; exit 1; }; \
	 s1=$$(date +%s); \
	 echo "       core tests passed in $$((s1 - s0))s -- OK"; \
	 echo "[6/10] test-native (+ examples, README, install, API docs)..."; \
	 s0=$$(date +%s); \
	 $(MAKE) --no-print-directory test-native || { echo "FAIL: test-native failed"; exit 1; }; \
	 s1=$$(date +%s); \
	 echo "       test-native passed in $$((s1 - s0))s -- OK"; \
	 echo "[7/10] test-api (8 layers: surface params differential roundtrip vectors kernels properties fuzz)..."; \
	 s0=$$(date +%s); \
	 $(MAKE) --no-print-directory test-api || { echo "FAIL: test-api failed"; exit 1; }; \
	 s1=$$(date +%s); \
	 echo "       test-api passed in $$((s1 - s0))s -- OK"; \
	 echo "[8/10] test-security (pen tests)..."; \
	 s0=$$(date +%s); \
	 $(MAKE) --no-print-directory test-security || { echo "FAIL: test-security failed"; exit 1; }; \
	 s1=$$(date +%s); \
	 echo "       test-security passed in $$((s1 - s0))s -- OK"; \
	 echo "[9/10] test-repl..."; \
	 s0=$$(date +%s); \
	 $(MAKE) --no-print-directory test-repl || { echo "FAIL: test-repl failed"; exit 1; }; \
	 s1=$$(date +%s); \
	 echo "       test-repl passed in $$((s1 - s0))s -- OK"; \
	 echo "[10/10] TLS & AEAD verification..."; \
	 s0=$$(date +%s); \
	 $(if $(PREPUSH_TLS), \
	   $(MAKE) --no-print-directory CONFIG_NATIVE_MODULES=y CONFIG_TLS=y test-tls test-tls-conn test-x509 test-crypto-aead || { echo "FAIL: TLS tests failed"; exit 1; }; \
	   s1=$$(date +%s); echo "        TLS tests passed in $$((s1 - s0))s -- OK", \
	   echo "        SKIPPED -- no OpenSSL >= 3.0"); \
	 t1=$$(date +%s); \
	 echo "================================================================="; \
	 echo "prepush: OK -- all 10 stages passed in $$((t1 - t0))s"; \
	 echo "================================================================="

# Installs prepush as .git/hooks/pre-push. The hook is NOT tracked by git, so
# a clone has no gate until somebody runs this -- which is why `make test`
# checks that it is installed and says so rather than assuming.
#
# Idempotent on purpose: the upgrade path (a hook already present, possibly an
# older version of this one) is a different program from the install path and
# is the one that ships unrun. It rewrites ours in place and refuses only a
# FOREIGN hook, so run this target twice -- the second run must not fail.
# The conformance number, machine-readable, from the SAME baseline the gate pins
# (dev.sh BASELINE) -- one source, so the published figure cannot drift from the
# gated one. Prints failures/total and the pass rate; does NOT run test262 (that
# is `./dev.sh t262`, which takes minutes).
conformance:
	@b=`sed -n 's/^BASELINE="\$${T262_BASELINE:-\([0-9]*\/[0-9]*\)}".*/\1/p' dev.sh`; \
	 f=$${b%%/*}; t=$${b##*/}; \
	 test -n "$$f" -a -n "$$t" || { echo "FAIL: no BASELINE in dev.sh"; exit 1; }; \
	 printf 'test262: %s failures / %s tests (%.4f%% pass), pinned in dev.sh\n' \
	   "$$f" "$$t" `awk "BEGIN{printf \"%.4f\", (1-$$f/$$t)*100}"`

# CycloneDX 1.6 for the CURRENT configuration. Half these components are
# config-gated, so a committed static SBOM rots on the first flag change --
# generate it at release time and attach it to the artifact instead.
sbom:
	@python3 tools/gen-sbom.py --timestamp "`date -u +%Y-%m-%dT%H:%M:%SZ`"

install-hooks:
	@test -d .git || { echo "FAIL: not a git working tree, no hooks to install"; exit 1; }
	@mkdir -p .git/hooks
	@if test -f .git/hooks/pre-push && \
	    ! grep -q 'dynajs-prepush-hook' .git/hooks/pre-push 2>/dev/null; then \
	  echo "FAIL: .git/hooks/pre-push exists and is not ours. Refusing to overwrite."; \
	  echo "      Move it aside, or add this line to it:  make prepush"; \
	  exit 1; \
	fi
	@printf '%s\n' \
	  '#!/bin/sh' \
	  '# dynajs-prepush-hook -- installed by `make install-hooks`. Do not edit;' \
	  '# edit the prepush target in the Makefile instead.' \
	  '#' \
	  '# A hook that exits 0 when it cannot run is not a hook, so this one fails' \
	  '# closed. To push past a known-red gate, and only then:  git push --no-verify' \
	  'set -e' \
	  'cd "$$(git rev-parse --show-toplevel)"' \
	  '# No backticks in the echo below: inside double quotes they are command' \
	  '# substitution, so the message itself ran the whole gate and exec ran it' \
	  '# a second time -- every push paid for two full runs.' \
	  'echo "pre-push: running make prepush (use --no-verify to skip)"' \
	  'exec make prepush' \
	  > .git/hooks/pre-push
	@chmod +x .git/hooks/pre-push
	@echo "install-hooks: .git/hooks/pre-push installed (runs \`make prepush\`)"

# A gate nobody installed is a gate nobody runs. This is a NOTICE, not a
# failure -- a fresh clone has no hook and must still be able to build.
check-hooks:
	@if test -d .git && ! grep -q 'dynajs-prepush-hook' .git/hooks/pre-push 2>/dev/null; then \
	  echo ""; \
	  echo "NOTICE: the pre-push gate is not installed. Run:  make install-hooks"; \
	  echo "        Without it nothing checks a push. See \`make prepush\`."; \
	  echo ""; \
	fi

test-native:
	@./dynajs$(EXE) -e 'import("dyna:mathx")' >/dev/null 2>&1 || { \
	  echo "FAIL: ./dynajs cannot load dyna:* modules."; \
	  echo "      Build one first: make clean && make CONFIG_NATIVE_MODULES=y"; \
	  exit 1; }
	@./tools/run-tests-parallel.sh $(NATIVE_TESTS) || exit 1
	@# dyna:log writes to stderr, which a process cannot read back: this one
	@# captures fd 2 in a subshell and asserts the line FORMAT.
	@sh tests/test_log_format.sh ./dynajs$(EXE) || exit 1
	@$(MAKE) --no-print-directory test-examples

# The dyna:* examples are RUN, not just shipped. All three of them had rotted
# silently against APIs this branch changed -- dyna:random.uuid was retired,
# Random stopped being a closable resource, and predictProba became rows x
# classes -- and nothing noticed, because an example that no one executes is
# documentation that no one proofreads.
test-examples:
	@./dynajs$(EXE) -e 'import("dyna:mathx")' >/dev/null 2>&1 || { \
	  echo "FAIL: ./dynajs cannot load dyna:* modules."; \
	  echo "      Build one first: make clean && make CONFIG_NATIVE_MODULES=y"; \
	  exit 1; }
	@for e in examples/js/dynajs_*.js; do \
	  $(WINE) ./dynajs$(EXE) $$e >/dev/null || { echo "FAIL: $$e"; exit 1; }; \
	  echo "  ok  $$e"; \
	done
	@echo "test-examples: every dyna:* example runs"
	@$(MAKE) --no-print-directory check-readme

# Same reason as test-examples, applied to the README: an example nobody runs is
# a confident-looking lie, and the README is the first thing anyone executes.
# Six of the sixteen blocks were wrong the first time this ran -- wrong argument
# order, a getter called as a method, f32 kernels handed f64 arrays -- none of
# which any other target would have caught.
# The README is gone (all .md docs removed except CLAUDE.md); skip loudly, same
# convention as check-api below.
check-readme:
	@if test -f README.md; then \
	  ./tools/check-readme-examples.sh README.md ./dynajs$(EXE); \
	else echo "check-readme: SKIPPED -- README.md absent"; fi
	@$(MAKE) --no-print-directory check-install

# install.sh is the first code most people run, and none of it is reachable from
# any other target. Two tests pin it: the Homebrew bootstrap (it can install
# software system-wide, so its refusals -- root, a failed download, a TRUNCATED
# download that must not be executed -- are the checks that matter), and the
# main flow, run TWICE so the upgrade path (a previous version being read and
# replaced) is exercised rather than shipped unrun. Each is proved load-bearing
# by injecting its removal.
check-install:
	@./tests/test_install_brew.sh ./install.sh
	@./tests/test_install_flow.sh ./install.sh
	@$(MAKE) --no-print-directory check-api

# The API reference had never had a single example executed, because its blocks
# are fragments that use the names their section imports -- so run each with its
# module's exports in scope, which is what a reader has. That found a whole
# duplicated section, functions from a deleted module, `path` documented as a
# string where the code refuses one, and a section of classes filed under the
# wrong module. Needs a CONFIG_NATIVE_MODULES=y build.
# docs/ is gitignored and LOCAL, so a fresh clone has none. Skip loudly rather
# than fail: a red gate everyone learns to ignore is worse than a stated gap.
check-api:
	@if test -f docs/dynajs-guide/API.md; then \
	  ./tools/check-api-examples.sh docs/dynajs-guide/API.md ./dynajs$(EXE); \
	else echo "check-api: SKIPPED -- docs/ absent (it is gitignored and local)"; fi
	@$(MAKE) --no-print-directory check-anchors
	@$(MAKE) --no-print-directory check-error-ids
	@$(MAKE) --no-print-directory check-types

# The ambient declarations for editors/IDEs must cover the binary's own export
# inventory and must typecheck; the API reference must name every export too.
# A skipped tsc is LOUD, never silent.
check-types:
	@python3 tools/check-dts-coverage.py || { echo "FAIL: types/dynajs.d.ts has drifted from the binary"; exit 1; }
	@python3 tools/check-api-coverage.py || { echo "FAIL: docs/dynajs-guide/API.md is missing an exported name"; exit 1; }
	@if command -v tsc >/dev/null 2>&1; then \
	  tsc --noEmit --strict --lib es2023 types/dynajs.d.ts; \
	else echo "check-types: SKIPPED -- tsc not installed (d.ts coverage verified, typecheck not)"; fi

# An error message that names a removed API is doc rot in the one place no
# example can reach. `Serializer.decode(...)` survived the class being deleted
# because every sweep looked at docs and tests, not at throw sites.
check-error-ids:
	@OBJDIR=$(OBJDIR) python3 tools/check-error-identifiers.py ./dynajs$(EXE)

# Every `](#...)` in the docs must resolve to a heading in the same file. A
# rename silently breaks them, and so does a heading edit. GitHub keeps `_` in
# an anchor -- a checker that strips it reports `#io_uring-...` as broken and
# invites a "fix" that breaks a working link.
check-anchors:
	@if test -f README.md; then \
	  if test -d docs/dynajs-guide; then \
	    python3 tools/check-anchors.py README.md docs/dynajs-guide/*.md \
	      && echo "check-anchors: every internal link resolves"; \
	  else python3 tools/check-anchors.py README.md \
	      && echo "check-anchors: README only -- docs/ absent (gitignored, local)"; fi; \
	else echo "check-anchors: SKIPPED -- README.md absent"; fi

# The dyn_path_borrow fallback in dyna-nat.c lives behind
# #ifndef CONFIG_NATIVE_MODULE_FILE, which NO ordinary build sets -- the flag is
# derived from src/dyna-file.c existing. So it had never been compiled, let
# alone linked or run, while csv/uring/ml/http all call through it. This target
# builds with the file module hidden and checks the fallback actually reports
# the real cause.
test-nofile:
	@mv src/dyna-file.c /tmp/dyna-file.c.hold
	@$(MAKE) --no-print-directory clean >/dev/null 2>&1; \
	 $(MAKE) --no-print-directory CONFIG_NATIVE_MODULES=y -j8 >/tmp/nofile.log 2>&1; \
	 rc=$$?; \
	 mv /tmp/dyna-file.c.hold src/dyna-file.c; \
	 [ $$rc -eq 0 ] || { echo "FAIL: build without dyna:file"; tail -20 /tmp/nofile.log; exit 1; }
	@./dynajs$(EXE) -e 'import("dyna:csv").then(m=>{try{new m.CSVFile("x")}catch(e){ \
	   if(!e.message.includes("dyna:file is not built in")) throw new Error("wrong message: "+e.message); \
	   print("  ok  the fallback names the real cause")}})'
	@echo "test-nofile: the no-dyna:file fallback compiles, links and reports correctly"
	@$(MAKE) --no-print-directory clean >/dev/null 2>&1

# dyna:uring is Linux + CONFIG_IO_URING only, so it cannot be exercised by
# test-native on a mac. This target builds the image that can. NB: on an arm64
# host the container is qemu-emulated and io_uring_queue_init returns ENOSYS --
# the test detects that and verifies the pread path and the Path-only argument
# contract, which is what the conversion changed.
test-uring:
	docker build --platform linux/amd64 --target uring-bench -f docker/Dockerfile -t dynajs:uringtest .
	docker run --rm --platform linux/amd64 dynajs:uringtest ./dynajs tests/test_uring_disk.js

stats: dynajs$(EXE)
	$(WINE) ./dynajs$(EXE) -qd

microbench: dynajs$(EXE)
	$(WINE) ./dynajs$(EXE) --std tests/microbench.js

# Profile-guided optimization, full flow: build instrumented -> run a training
# set -> merge the profile -> rebuild optimized. Override the training workload
# with PGO_TRAIN="a.js b.js". On macOS llvm-profdata is behind xcrun.
LLVM_PROFDATA?=$(shell command -v llvm-profdata 2>/dev/null || echo xcrun llvm-profdata)
PGO_TRAIN?=tests/microbench.js tests/bench_array_ext.js
PGO_MAKE=$(MAKE) CONFIG_NATIVE_MODULES=y $(if $(CONFIG_MIMALLOC),CONFIG_MIMALLOC=y) $(if $(CONFIG_LTO),CONFIG_LTO=y)
pgo:
	$(MAKE) clean
	$(PGO_MAKE) CONFIG_PGO_GEN=y dynajs$(EXE)
	rm -rf pgo-data pgo.profdata
	for t in $(PGO_TRAIN); do LLVM_PROFILE_FILE="pgo-data/%p-%m.profraw" ./dynajs$(EXE) $$t >/dev/null 2>&1 || true; done
	$(LLVM_PROFDATA) merge -o pgo.profdata pgo-data/*.profraw
	$(MAKE) clean
	$(PGO_MAKE) CONFIG_PGO_USE=y dynajs$(EXE)
	@echo "PGO build complete (trained on: $(PGO_TRAIN))"

# BOLT (post-link binary optimization) is Linux-only and needs a perf sample:
#   perf record -e cycles:u -j any,u -o perf.data -- ./dynajs <workload>
#   perf2bolt -p perf.data -o dynajs.fdata ./dynajs
#   llvm-bolt ./dynajs -o dynajs.bolt -data=dynajs.fdata \
#     -reorder-blocks=ext-tsp -reorder-functions=hfsort+ -split-functions -icf=1
bolt-help:
	@echo "BOLT is a Linux post-link step; see the recipe comment above 'bolt-help' in the Makefile."

ifeq ($(wildcard test262/features.txt),)
test2-bootstrap:
	git clone --single-branch --shallow-since=$(TEST262_SINCE) https://github.com/tc39/test262.git
	(cd test262 && git checkout -q $(TEST262_COMMIT) && patch -p1 < ../tests/test262.patch && cd ..)
else
test2-bootstrap:
	(cd test262 && git fetch && git reset --hard $(TEST262_COMMIT) && patch -p1 < ../tests/test262.patch && cd ..)
endif

ifeq ($(wildcard test262/features.txt),)
test2 test2-update test2-default test2-check:
	@echo test262 tests not installed
else
# Test262 tests
test2-default: run-test262
	time ./run-test262 -t -m -c tools/test262.conf

test2: run-test262
	time ./run-test262 -t -m -c tools/test262.conf -a

test2-update: run-test262
	./run-test262 -t -u -c tools/test262.conf -a

test2-check: run-test262
	time ./run-test262 -t -m -c tools/test262.conf -E -a
endif

tests/bjson.so: $(OBJDIR)/tests/bjson.pic.o
	$(CC) $(LDFLAGS) -shared -o $@ $^ $(LIBS)

-include $(wildcard $(OBJDIR)/*.d)
