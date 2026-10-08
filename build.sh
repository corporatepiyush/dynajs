#!/usr/bin/env bash
# ============================================================================
# build.sh — the one build/test/prove/entry-point script for DynaJS.
# Self-contained: the whole engine (config, flags, parallel compile, links,
# codegen) runs right here. No external build system.
#
# QUICK START
#   ./build.sh build CONFIG_NATIVE_MODULES=y CONFIG_TLS=y  # full hardened build -> ./dynajs
#   ./build.sh rebuild CONFIG_NATIVE_MODULES=y             # clean + build
#   ./build.sh build-fast                                  # -O0 fast edit loop (own objdir)
#   MODULES=csv,crypto ./build.sh test-native              # scoped test run (a scope is mandatory)
#   ./build.sh test-blackbox bb_cli                        # one black-box contract suite
#   ./build.sh test-module csv                             # one module's native suites
#   ./build.sh gate                                        # full 12-stage proof (test262 included)
#   ./build.sh --src /tmp/ws/src --out /tmp/ws/out build CONFIG_NATIVE_MODULES=y
#                                                          # workspace copy: sources elsewhere,
#                                                          #   artifacts contained
#
# GLOBAL OPTIONS / KNOBS
#   --src DIR    source tree playing the role of src/     (default: src; env SRC=)
#   --out DIR    artifact root: objects, binaries, libs,  (default: .obj; env OUT=)
#                generated C; variants nest beneath it
#   MODULES=a,b  scope the test matrix to those modules   (TEST_SCOPE=all = full)
#   DEV_HARDEN_CFG  build config appended to every build  (default "CONFIG_HARDEN=y
#                    CONFIG_WERROR=y"; set DEV_HARDEN_CFG="" to opt out)
#
#   ./build.sh help — the complete subcommand list; failure diagnostics come from
#   each leg's own output (full log path printed on any FAIL).
# ============================================================================

# build.sh — one entry point for building, testing and profiling dynascript, so we
# stop hand-writing shell. Output is terse: "<stage>: ok" or "FAIL: <why>" plus a
# nonzero exit on any failure. Bare `./build.sh` prints this list. (dev.sh is a
# one-line shim that execs this file -- kept for muscle memory and old callers.)
#
# THE BUILD IS NATIVE NOW: the CONFIG_*/flags/object/parallel/link logic that
# used to live in the Makefile runs right here (see "BUILD ENGINE" below), and
# build.sh calls no external build system. Phase 1 ported the CONFIG_*/flags/
# object/parallel/link logic; phase 2 (below, "PHASE 2 -- every remaining
# Makefile target") ported the test matrix, the slim/one minimal binaries, the
# fuzz proof, the check gates, install and the prepush dispatch.
#
#   ./build.sh build [KEY=VALUE...]    0-warning build (fails on ANY warning);
#                                      CONFIG_HARDEN=y CONFIG_WERROR=y by
#                                      default -- opt out with DEV_HARDEN_CFG=""
#                                      or append CONFIG_HARDEN= CONFIG_WERROR=
#                                      as args. Any CONFIG_* the old Makefile
#                                      took is accepted: CONFIG_NATIVE_MODULES=y
#                                      CONFIG_TLS=y CONFIG_FASTDEV=y CONFIG_ASAN=y
#                                      CONFIG_LTO=y CONFIG_NATIVE=y CONFIG_USING=y ...
#   ./build.sh build-fast              CONFIG_FASTDEV=y native (-O0) build -- the
#                                      edit/build/test loop knob, not a measured build
#   ./build.sh rebuild                 clean + build
#   ./build.sh run FILE [args...]      incremental build, then run a JS file
#   ./build.sh test [MODULES=a,b]      PHASE 2: the native test matrix ports here;
#                                      builds the right binary, then says so
#   ./build.sh test-all                PHASE 2 (same, forced full matrix)
#   ./build.sh test-core               the engine-core JS suites leg
#   ./build.sh test-native             PHASE 2 stub -- errors by design for now
#   ./build.sh test-blackbox [f...]    tests/blackbox/run.sh ./dynajs; one run per
#                                      bb_* substring filter (no filter: every suite)
#   ./build.sh test-module M           PHASE 2 (MODULES=M scoped native suites)
#   ./build.sh modules-changed [BASE]  changed files -> module names (the scope
#                                      `test`/`gate` derive from)
#   ./build.sh quick [FILE...]         native build + core sanity suites
#                                      (builtin/language/encoding/json/crypto), then FILEs
#   ./build.sh mod csv,net             PHASE 2 (slim-binary module tests)
#   ./build.sh one MODULE [--build-only]  PHASE 2 (one module's minimal binary)
#   ./build.sh ccheck [FILE]           single-TU compile check (default: modified src/*.c)
#   ./build.sh check-objs              the CONFIG_CHECK_JSVALUE check objects
#   ./build.sh ws SUBCMD               concurrent workspaces: create|list|diff|merge|
#                                      reset|drop|shell (delegates to tools/ws.sh)
#   ./build.sh asan FILE|test          ASan build + run (warns if the file needs dyna:*)
#   ./build.sh ubsan FILE|test         UBSan build + run (same warning)
#   ./build.sh openlibm FILE|test      CONFIG_OPENLIBM build + run (needs
#                                      third_party/openlibm/libopenlibm.a)
#   ./build.sh t262 [SUBTREE]          run test262 (subtree, else full run vs the pin)
#   ./build.sh bench FILE [args...]    CONFIG_NATIVE build + run (NO dyna:* modules)
#   ./build.sh status                  toolchain, dependencies, build info &
#                                      the derived modules-changed scope
#   ./build.sh check                   core-purity + doc-lint + codegraph checks
#   ./build.sh rss FILE [N...]         peak-RSS-plateau leak check (FILE reads
#                                      scriptArgs[1]=N; default Ns 20000 100000 500000)
#   ./build.sh amd64                   docker x86 SIMD verify (Dockerfile target: amd64)
#   ./build.sh gate [TEST.js...]       THE proof gate -- the 12-stage wave proof
#                                      (absorbed from tools/prepush-parallel.sh,
#                                      now deleted) plus the test262 corpus leg:
#                                      codegraph/defects/imports lints beside the
#                                      clean+build, then t262 FULL, then fuzz/
#                                      core/native/api/security/repl/tls/blackbox
#                                      on a 3-worker pool. `prepush` is an alias.
#   ./build.sh clean                   remove objdirs + binaries + generated artifacts
#
# Knobs: DEV_JOBS (test fan-out budget), DEV_BUILD_J (compile pool size, default
# nproc), DEV_TIMEOUT (per-stage wall seconds, 0 disables, default 1800),
# DEV_SERIAL=1 (one stage at a time, in-tree), DEV_CCACHE=0 (disable ccache wrap),
# DEV_HARDEN_CFG (build-config string appended to every build.sh build; default
# "CONFIG_HARDEN=y CONFIG_WERROR=y", set it to "" to build unhardened),
# DEVSH_DEBUG=1 (echo every wrapped command + rc + elapsed as "[devsh]" stderr
# lines), DEV_PROGRESS_LOG (stage journal, default $OUT_BASE/.dev/progress.log),
# T262_BASELINE (full-t262 failure pin, default 58/83744), DEV_TREES (gate clone
# tree root, default $OUT_BASE/.dev/trees-$$), DEV_STATE (private per-tree state
# root, default $OUT_BASE/.dev), DEV_LOCK_TIMEOUT (seconds to wait for the
# per-tree build lock before failing, default 900), DEV_LOCK_STALE (age at
# which a live-looking lock is broken anyway, default 3600; 0 disables),
# MODULES (comma/space module list: scope test/gate to those modules' suites),
# TEST_SCOPE=all (the deliberate full-matrix escape -- skips the derivation).
# Gate knobs: PREPUSH_MODULES (gate scope override), PREPUSH_TIMEOUT (per-stage
# wall seconds, default 1800), PREPUSH_PAR (wave-B workers, default 3),
# PREPUSH_J (build -j), PREPUSH_DEBUG=1 (echo every wrapped command), and
# PREPUSH_MULTI_TESTS (the full native matrix's cross-module override).
#
# Exit codes / failure UX: rc=0 all stages ok; rc=1 first FAIL: (with diagnosis
# hints: rc=124 = hit the DEV_TIMEOUT wall — a hang, not a crash; rc=139 =
# segfault, rerun './build.sh asan FILE' for a stack). Failed-stage logs are kept
# and their path printed ("full log: ...").
#
# Gotchas:
# - asan/ubsan/openlibm/bench builds set NO CONFIG_NATIVE_MODULES: every dyna:*
#   import fails or silently skips — the script warns and prints the rebuild fix.
# - switching configs auto-wipes that config's objdir (stamp .obj/.config-sig,
#   the CONFIG_SIG port) and relinks the shared binaries (.build-variant).
# - `gate` runs IN THIS TREE: its build leg is clean + build, so it must never
#   run concurrently with your own build -- one .obj, one owner.
# - TLS gate legs are SKIPPED, not failed, without OpenSSL >= 3 in pkg-config.
set -uo pipefail
cd "$(dirname "$0")" || exit 2
ROOT=$PWD

NCPU=$( (command -v nproc >/dev/null 2>&1 && nproc) || sysctl -n hw.ncpu 2>/dev/null || echo 4 )
JOBS=${DEV_BUILD_J:-${DEV_JOBS:-$NCPU}}
PAR=${DEV_PAR:-$(( JOBS / 3 > 1 ? JOBS / 3 : 1 ))}
TIMEOUT=${DEV_TIMEOUT:-1800}
SERIAL=${DEV_SERIAL:-0}
USE_CCACHE=${DEV_CCACHE:-1}
DEBUG=${DEVSH_DEBUG:-0}
PROGRESS_LOG="${DEV_PROGRESS_LOG:-}"
DEV_STATE="${DEV_STATE:-}"          # default: $OUT_BASE/.dev (private, in-tree)

# T262 conformance pin. MEASURED, not inherited: before B1-05 the gate never
# built .obj-t262/run-test262, so this number came from whatever binary happened
# to be lying in that objdir. Once the gate built the runner itself, the first
# honest full run measured 141/83744 -- i.e. 83 failures had accumulated with
# the gate still reporting "58 ok" off an Oct-1 binary.
#
# 141 is the TRUTH on workspace base (branch audit-hardening). Lowering the pin
# to the measured value is the only way to keep the pin a REGRESSION detector:
# pinning a number no build produces would make the leg permanently red, and
# keeping the stale 58 would leave it permanently green-but-wrong. TICKET:
# B1-05/t262-pin-rebased -- 141 conformance gaps are open against dynajs.d.ts
# and belong to the engine lanes; this pin now makes any FURTHER regression
# visible, which is what a pin is for.
BASELINE="${T262_BASELINE:-141/83744}"
CONF=tools/test262.conf

OPENSSL_PC="$(brew --prefix openssl@3 2>/dev/null || brew --prefix openssl 2>/dev/null)/lib/pkgconfig"
GATE_TLS=""
PKG_CONFIG_PATH="$OPENSSL_PC:${PKG_CONFIG_PATH:-}" \
  pkg-config --exists 'openssl >= 3.0' 2>/dev/null && GATE_TLS="y"
# Hardened + strict is the DEFAULT for every build.sh build: per-flag-probed
# security codegen (CONFIG_HARDEN, probed right here in the engine now) and
# -Werror. Opt out with DEV_HARDEN_CFG="" or by appending CONFIG_HARDEN=
# CONFIG_WERROR= to the build args.
HARDEN_CFG=${DEV_HARDEN_CFG-"CONFIG_HARDEN=y CONFIG_WERROR=y"}
NATIVE_CFG="CONFIG_NATIVE_MODULES=y"
[ "$GATE_TLS" = y ] && NATIVE_CFG="$NATIVE_CFG CONFIG_TLS=y"
[ -n "$HARDEN_CFG" ] && NATIVE_CFG="$NATIVE_CFG $HARDEN_CFG"

die(){ echo "FAIL: $*" >&2; exit 1; }
have(){ command -v "$1" >/dev/null 2>&1; }
need_file(){ [ -f "$1" ] || die "${cmd:-run}: no such file: $1 (paths are relative to the repo root: $ROOT)"; }

_log_progress(){
  mkdir -p "$(dirname "$PROGRESS_LOG")" 2>/dev/null
  echo "$(date +%H:%M:%S) $*" >> "$PROGRESS_LOG" 2>/dev/null || true
}

_now(){ perl -MTime::HiRes=time -e 'print time' 2>/dev/null \
          || python3 -c 'import time; print(time.time())' 2>/dev/null \
          || date +%s; }
_elapsed(){ awk -v a="$1" -v b="$2" 'BEGIN{printf "%.2f", b-a}'; }
_v(){
  if [ "$DEBUG" != 1 ]; then "$@"; return $?; fi
  local t0 rc
  t0=$(_now)
  echo "[devsh] \$ $*" >&2
  "$@"; rc=$?
  echo "[devsh] rc=$rc $(_elapsed "$t0" "$(_now)")s: $*" >&2
  return "$rc"
}

CCACHE_CC=""
if [ "$USE_CCACHE" != "0" ] && have ccache; then
  if have clang; then CCACHE_CC="ccache clang"
  elif have gcc; then CCACHE_CC="ccache gcc"
  fi
fi

# ============================================================================
# BUILD ENGINE -- the native build system. The old Makefile was the spec; every
# layer below is a direct port:
#   (1) eng_parse_cfg / eng_detect  -- the CONFIG_* space + uname/CROSS_PREFIX
#   (2) eng_flags                   -- CFLAGS/LDFLAGS assembly, incl. the
#                                       CONFIG_HARDEN probe block and its
#                                       .obj/.harden-flags cache
#   (3) eng_ensure                  -- the CONFIG_SIG stamp + variant stamp
#   (4) eng_objects                 -- CORE_OBJS / DYNAJS_LIB_OBJS /
#                                       NAT_MODULE_OBJS / DYNAJS_OBJS
#   (5) eng_pool                    -- dependency-tracked parallel compile
#                                       (-MMD .d parsing, config-stamp wipe)
#   (6) engine_build                -- dynajsc link -> codegen (repl.c,
#                                       hello.c, test_fib.c) -> dynajs /
#                                       libdynajs.a / run-test262 / examples
# ============================================================================

ENG_CONTRACT_LOG=${DEV_CONTRACT_LOG:-}
eng_progress(){
  [ -n "$ENG_CONTRACT_LOG" ] || return 0
  mkdir -p "$(dirname "$ENG_CONTRACT_LOG")" 2>/dev/null
  echo "$(date '+%H:%M:%S') core: $1" >> "$ENG_CONTRACT_LOG" 2>/dev/null || true
}

ENG_CFG_KEYS="NATIVE_MODULES TLS ASAN MSAN UBSAN TSAN FASTDEV HARDEN WERROR IO_URING SQLITE ZSTD OPENLIBM SYSTEMLIBM MIMALLOC LTO PGO_GEN PGO_USE CLANG COSMO FREEBSD WIN32 M32 NATIVE PROP_HASH_MIX OBJ_POOL NURSERY_PROBE PROFILE LITERAL_BOILERPLATE FAST_PROP_TEARDOWN USING ML_NO_SIMD PREFIX CROSS_PREFIX OPENLIBM_DIR SHARED_LIBS FUZZ_NO_DEFAULT_SAN"

cfg_v(){ local n="CFG_$1"; printf '%s' "${!n-}"; }
cfg_set(){ printf -v "CFG_$1" '%s' "$2"; }

# (1) config parse: env CONFIG_* first, then KEY=VALUE args (last one wins, and
# an empty value is a real value -- `CONFIG_HARDEN= build` opts out, exactly
# like `CONFIG_HARDEN= ./build.sh build`).
ENG_CFG_DESC=""
eng_parse_cfg(){
  local k en v a
  # reset first: engine_build may run several times in one process (gate), and
  # a key set by an earlier config must not leak into the next one.
  for k in $ENG_CFG_KEYS; do cfg_set "$k" ""; done
  for k in $ENG_CFG_KEYS; do
    en="CONFIG_$k"; v="${!en-}"
    [ -n "$v" ] && cfg_set "$k" "$v"
  done
  ENG_CFG_DESC=""
  for a in "$@"; do
    case "$a" in
      [A-Za-z_]*=*)
        k=${a%%=*}; v=${a#*=}
        case "$k" in CONFIG_*) k=${k#CONFIG_} ;; esac
        case " $ENG_CFG_KEYS " in
          *" $k "*) ;;
          *) die "build: unknown config key '$k' (known:$(for x in $ENG_CFG_KEYS; do printf ' %s' "$x"; done))" ;;
        esac
        cfg_set "$k" "$v"
        ENG_CFG_DESC="$ENG_CFG_DESC CONFIG_$k=$v" ;;
      *) die "build: not a KEY=VALUE config argument: '$a'" ;;
    esac
  done
  ENG_CFG_DESC="${ENG_CFG_DESC# }"
}

# (1b) platform detection: the Makefile's uname block + OBJDIR selection.
eng_detect(){
  ENG_UNAME_S=$(uname -s 2>/dev/null || echo unknown)
  ENG_UNAME_M=$(uname -m 2>/dev/null || echo unknown)
  [ "$ENG_UNAME_S" = Darwin ]  && cfg_set DARWIN y
  [ "$ENG_UNAME_S" = FreeBSD ] && cfg_set FREEBSD y
  # Darwin/FreeBSD force clang + plain ar; FreeBSD and cosmo force LTO off.
  if [ -n "$(cfg_v DARWIN)" ] || [ -n "$(cfg_v FREEBSD)" ]; then
    cfg_set CLANG y; cfg_set DEFAULT_AR y
    [ -n "$(cfg_v FREEBSD)" ] && cfg_set LTO ""
  fi
  [ -n "$(cfg_v COSMO)" ] && cfg_set LTO ""
  # SYSTEMLIBM: default on only for Darwin-arm64, =n disables, =y forces elsewhere.
  if [ -z "$(cfg_v SYSTEMLIBM)" ] && [ -z "$(cfg_v WIN32)" ] && [ -z "$(cfg_v COSMO)" ] \
     && [ "$ENG_UNAME_S-$ENG_UNAME_M" = Darwin-arm64 ]; then
    cfg_set SYSTEMLIBM y
  fi
  # WIN32: explicit, or an MSYSTEM (MSYS2) shell implies it.
  if [ -z "$(cfg_v WIN32)" ] && [ -n "${MSYSTEM-}" ]; then cfg_set WIN32 y; fi
  if [ -n "$(cfg_v WIN32)" ]; then
    ENG_EXE=.exe
    if [ -z "$(cfg_v CROSS_PREFIX)" ]; then
      if [ -n "$(cfg_v M32)" ]; then cfg_set CROSS_PREFIX i686-w64-mingw32-
      else cfg_set CROSS_PREFIX x86_64-w64-mingw32-; fi
    fi
  else
    ENG_EXE=""
  fi
  ENG_CROSS_PREFIX="$(cfg_v CROSS_PREFIX)"
  # OBJDIR: shared root (--out moves it), then a private subdir per variant --
  # sanitizers, then fastdev, then using (the Makefile's append order).
  OBJDIR=$OUT_BASE
  [ -n "$(cfg_v ASAN)" ]  && OBJDIR="$OBJDIR/asan"
  [ -n "$(cfg_v MSAN)" ]  && OBJDIR="$OBJDIR/msan"
  [ -n "$(cfg_v UBSAN)" ] && OBJDIR="$OBJDIR/ubsan"
  [ -n "$(cfg_v TSAN)" ]  && OBJDIR="$OBJDIR/tsan"
  [ -n "$(cfg_v FASTDEV)" ] && OBJDIR="$OBJDIR/fastdev"
  [ "$(cfg_v USING)" = y ] && OBJDIR="$OBJDIR/using"
  # SHARED_LIBS: auto on non-cosmo/darwin/win32 unless the user set it.
  if [ -z "$(cfg_v SHARED_LIBS)" ] && [ -z "$(cfg_v COSMO)" ] \
     && [ -z "$(cfg_v DARWIN)" ] && [ -z "$(cfg_v WIN32)" ]; then
    cfg_set SHARED_LIBS y
  fi
  ENG_PREFIX="$(cfg_v PREFIX)"; [ -n "$ENG_PREFIX" ] || ENG_PREFIX=/usr/local
  ENG_VERSION=$(cat VERSION 2>/dev/null || echo unknown)
  # minimal compiler pick (eng_flags refines it with the ccache wrap); ccheck
  # uses this path without assembling full flags.
  if [ -n "$(cfg_v COSMO)" ]; then ENG_CC=cosmocc; ENG_HOST_CC=gcc
  elif [ -n "$(cfg_v CLANG)" ]; then ENG_CC="${ENG_CROSS_PREFIX}clang"; ENG_HOST_CC=clang
  else ENG_CC="${ENG_CROSS_PREFIX}gcc"; ENG_HOST_CC=gcc; fi
}

# (2) flag assembly. Order mirrors the Makefile's textual append order, which
# matters for include paths: -I. -Isrc first, then per-library -I flags.
eng_flags(){
  local anysan=0 fort="" f key cache flags fortline
  # every optional accumulation starts empty: a no-TLS/no-sanitizer/no-native
  # build must not trip `set -u` on a variable the TLS path happens to set.
  ENG_EXTRA_LIBS=""; ENG_OPENSSL_CFLAGS=""; ENG_OPENSSL_LIBS=""
  ENG_SQLITE_CFLAGS=""; ENG_SQLITE_LIBS=""; ENG_ZSTD_CFLAGS=""; ENG_ZSTD_LIBS=""
  ENG_BROTLI_CFLAGS=""; ENG_BROTLI_LIBS=""; ENG_URING_LIB=""
  eng_ldflags_early=""; eng_o="-O2"; eng_lto=""
  ENG_OPENLIBM_A=""
  ENG_HARDEN_FORTIFY=""; ENG_HARDEN_LDFLAGS=""; ENG_HARDEN_FLAGS=""
  # --- compiler + AR selection --------------------------------------------
  if [ -n "$(cfg_v CLANG)" ]; then
    ENG_HOST_CC=clang
    if [ -n "$CCACHE_CC" ]; then ENG_CC="$CCACHE_CC"; else ENG_CC="${ENG_CROSS_PREFIX}clang"; fi
    ENG_CFLAGS="-g -Wall -Wextra -Wno-sign-compare -Wno-missing-field-initializers -Wundef -Wuninitialized -Wunused -Wno-unused-parameter -Wwrite-strings -Wchar-subscripts -funsigned-char"
    if [ -n "$(cfg_v DEFAULT_AR)" ]; then ENG_AR="${ENG_CROSS_PREFIX}ar"
    elif [ -n "$(cfg_v LTO)" ]; then ENG_AR="${ENG_CROSS_PREFIX}llvm-ar"
    else ENG_AR="${ENG_CROSS_PREFIX}ar"; fi
  elif [ -n "$(cfg_v COSMO)" ]; then
    ENG_HOST_CC=gcc; ENG_CC=cosmocc
    ENG_CFLAGS="-g -Wall -Wno-array-bounds -Wno-format-truncation"
    ENG_AR=cosmoar
  else
    ENG_HOST_CC=gcc
    if [ -n "$CCACHE_CC" ] && [ "${CCACHE_CC#ccache }" != "$CCACHE_CC" ]; then
      ENG_CC="$CCACHE_CC"
    else
      ENG_CC="${ENG_CROSS_PREFIX}gcc"
    fi
    ENG_CFLAGS="-g -Wall -Wno-array-bounds -Wno-format-truncation -Wno-infinite-recursion"
    if [ -n "$(cfg_v LTO)" ]; then ENG_AR="${ENG_CROSS_PREFIX}gcc-ar"; else ENG_AR="${ENG_CROSS_PREFIX}ar"; fi
  fi
  # --- -m32 ----------------------------------------------------------------
  if [ -n "$(cfg_v M32)" ]; then
    ENG_CFLAGS="$ENG_CFLAGS -msse2 -mfpmath=sse"
    if [ -z "$(cfg_v WIN32)" ]; then
      ENG_CFLAGS="$ENG_CFLAGS -m32"
      eng_ldflags_early="-m32"
    fi
  fi
  ENG_CFLAGS="$ENG_CFLAGS -std=gnu17 -fwrapv"
  [ "$(cfg_v WERROR)" = y ] && ENG_CFLAGS="$ENG_CFLAGS -Werror"
  # --- defines -------------------------------------------------------------
  ENG_DEFINES="-D_GNU_SOURCE -DCONFIG_VERSION=\"$ENG_VERSION\""
  [ -n "$(cfg_v WIN32)" ] && ENG_DEFINES="$ENG_DEFINES -D__USE_MINGW_ANSI_STDIO"
  if [ -z "$(cfg_v WIN32)" ] && "$ENG_CC" -o /dev/null "$SRC_DIR/compat/test-closefrom.c" 2>/dev/null; then
    ENG_DEFINES="$ENG_DEFINES -DHAVE_CLOSEFROM"
  fi
  ENG_CFLAGS="$ENG_CFLAGS $ENG_DEFINES -I. -I$SRC_DIR"
  [ "$GEN_PFX" = "" ] || ENG_CFLAGS="$ENG_CFLAGS -I$OUT_BASE"  # generated headers (libunicode-table.h) under --out
  # --- default-on engine flags (each with its Makefile opt-out) ------------
  [ "$(cfg_v PROP_HASH_MIX)" != n ] && ENG_CFLAGS="$ENG_CFLAGS -DCONFIG_PROP_HASH_MIX"
  [ "$(cfg_v USING)" = y ] && ENG_CFLAGS="$ENG_CFLAGS -DCONFIG_USING"
  [ "$(cfg_v OBJ_POOL)" = y ] && ENG_CFLAGS="$ENG_CFLAGS -DCONFIG_OBJ_POOL"
  [ "$(cfg_v NURSERY_PROBE)" = y ] && ENG_CFLAGS="$ENG_CFLAGS -DCONFIG_NURSERY_PROBE"
  eng_lb=1; [ "$(cfg_v LITERAL_BOILERPLATE)" = n ] && eng_lb=0
  eng_ft=1; [ "$(cfg_v FAST_PROP_TEARDOWN)" = n ] && eng_ft=0
  ENG_CFLAGS="$ENG_CFLAGS -DCONFIG_LITERAL_BOILERPLATE=$eng_lb -DCONFIG_FAST_PROP_TEARDOWN=$eng_ft"
  [ "$(cfg_v SYSTEMLIBM)" = y ] && ENG_CFLAGS="$ENG_CFLAGS -DCONFIG_SYSTEMLIBM"
  # --- optimization levels -------------------------------------------------
  if [ -n "$(cfg_v FASTDEV)" ]; then eng_o="-O0"; else eng_o="-O2"; fi
  ENG_CFLAGS_NOLTO="$ENG_CFLAGS $eng_o"
  case "$ENG_UNAME_M" in
    x86_64|amd64|i?86) eng_native_flag="-march=native" ;;
    *)                 eng_native_flag="-mcpu=native" ;;
  esac
  [ -n "$(cfg_v NATIVE)" ] && ENG_CFLAGS_NOLTO="$ENG_CFLAGS_NOLTO $eng_native_flag"
  # --- LDFLAGS base --------------------------------------------------------
  if [ -n "$(cfg_v COSMO)" ]; then ENG_LDFLAGS="-s"; else ENG_LDFLAGS="-g"; fi
  [ -n "${eng_ldflags_early-}" ] && ENG_LDFLAGS="$eng_ldflags_early $ENG_LDFLAGS"
  # --- post-OPT-snapshot CFLAGS appends (NOT in CFLAGS_NOLTO) --------------
  [ -n "$(cfg_v PROFILE)" ] && ENG_CFLAGS="$ENG_CFLAGS -p"
  if [ -n "$(cfg_v MIMALLOC)" ]; then
    ENG_CFLAGS="$ENG_CFLAGS -DCONFIG_MIMALLOC -Ithird_party/mimalloc/include"
  fi
  [ -n "$(cfg_v PGO_GEN)" ] && {
    ENG_CFLAGS="$ENG_CFLAGS -fprofile-generate=pgo-data"
    ENG_LDFLAGS="$ENG_LDFLAGS -fprofile-generate=pgo-data"; }
  [ -n "$(cfg_v PGO_USE)" ] && {
    ENG_CFLAGS="$ENG_CFLAGS -fprofile-use=pgo.profdata -Wno-profile-instr-out-of-date -Wno-profile-instr-unprofiled -Wno-backend-plugin"
    ENG_LDFLAGS="$ENG_LDFLAGS -fprofile-use=pgo.profdata"; }
  DYNAJSC_EXTRA_CFLAGS=""
  [ -n "$(cfg_v ASAN)" ] && {
    anysan=1
    ENG_CFLAGS="$ENG_CFLAGS -fsanitize=address -fno-omit-frame-pointer"
    ENG_LDFLAGS="$ENG_LDFLAGS -fsanitize=address -fno-omit-frame-pointer"
    DYNAJSC_EXTRA_CFLAGS="$DYNAJSC_EXTRA_CFLAGS -fsanitize=address"; }
  [ -n "$(cfg_v MSAN)" ] && {
    anysan=1
    ENG_CFLAGS="$ENG_CFLAGS -fsanitize=memory -fno-omit-frame-pointer"
    ENG_LDFLAGS="$ENG_LDFLAGS -fsanitize=memory -fno-omit-frame-pointer"; }
  [ -n "$(cfg_v UBSAN)" ] && {
    anysan=1
    ENG_CFLAGS="$ENG_CFLAGS -fsanitize=undefined -fno-omit-frame-pointer"
    ENG_LDFLAGS="$ENG_LDFLAGS -fsanitize=undefined -fno-omit-frame-pointer"
    DYNAJSC_EXTRA_CFLAGS="$DYNAJSC_EXTRA_CFLAGS -fsanitize=undefined"; }
  [ -n "$(cfg_v TSAN)" ] && {
    anysan=1
    ENG_CFLAGS="$ENG_CFLAGS -fsanitize=thread -fno-omit-frame-pointer"
    ENG_LDFLAGS="$ENG_LDFLAGS -fsanitize=thread -fno-omit-frame-pointer"
    DYNAJSC_EXTRA_CFLAGS="$DYNAJSC_EXTRA_CFLAGS -fsanitize=thread"; }
  export DYNAJSC_EXTRA_CFLAGS
  # --- CONFIG_TLS ----------------------------------------------------------
  if [ -n "$(cfg_v TLS)" ]; then
    local pc
    pc="$(brew --prefix openssl@3 2>/dev/null || brew --prefix openssl 2>/dev/null)/lib/pkgconfig"
    ENG_OPENSSL_CFLAGS=$(PKG_CONFIG_PATH="$pc:${PKG_CONFIG_PATH:-}" pkg-config --cflags 'openssl >= 3.0' 2>/dev/null)
    ENG_OPENSSL_LIBS=$(PKG_CONFIG_PATH="$pc:${PKG_CONFIG_PATH:-}" pkg-config --libs 'openssl >= 3.0' 2>/dev/null)
    if [ -z "$ENG_OPENSSL_LIBS" ]; then
      die "CONFIG_TLS=y needs OpenSSL >= 3.0. Install it (brew install openssl@3, \
apt-get install libssl-dev, apk add openssl-dev) or set PKG_CONFIG_PATH. \
Note /usr/bin/openssl on macOS is LibreSSL and does not satisfy this."
    fi
    ENG_CFLAGS="$ENG_CFLAGS -DCONFIG_TLS $ENG_OPENSSL_CFLAGS"
    ENG_EXTRA_LIBS="$ENG_OPENSSL_LIBS"
  fi
  # --- CONFIG_NATIVE_MODULES ------------------------------------------------
  ENG_SQLITE_LIBS=""
  if [ -n "$(cfg_v NATIVE_MODULES)" ]; then
    ENG_CFLAGS="$ENG_CFLAGS -DCONFIG_NATIVE_MODULES"
    local m up
    # The CFLAGS define set (exactly the Makefile's list -- note protobuf/asn1
    # have objects but no -D here, as in the Makefile).
    for m in random compress stream async net structures structures3 ml simd file \
             semver bytes crypto matcher encoding time mathx csv dataframe uuid \
             config log url term validate json schema xml yaml decimal vserialize \
             html sys scrape oauth2 bench; do
      if [ -f "$SRC_DIR/dyna-$m.c" ]; then
        up=$(printf '%s' "$m" | tr 'a-z-' 'A-Z_')
        ENG_CFLAGS="$ENG_CFLAGS -DCONFIG_NATIVE_MODULE_$up"
      fi
    done
    local spc zpc bpc
    spc="$(brew --prefix sqlite 2>/dev/null)/lib/pkgconfig"
    ENG_SQLITE_CFLAGS=$(PKG_CONFIG_PATH="$spc:${PKG_CONFIG_PATH:-}" pkg-config --cflags sqlite3 2>/dev/null)
    ENG_SQLITE_LIBS=$(PKG_CONFIG_PATH="$spc:${PKG_CONFIG_PATH:-}" pkg-config --libs sqlite3 2>/dev/null)
    if [ -n "$ENG_SQLITE_LIBS" ]; then
      ENG_CFLAGS="$ENG_CFLAGS -DCONFIG_SQLITE $ENG_SQLITE_CFLAGS"
      ENG_EXTRA_LIBS="$ENG_EXTRA_LIBS $ENG_SQLITE_LIBS"
    fi
    zpc="$(brew --prefix zstd 2>/dev/null)/lib/pkgconfig"
    ENG_ZSTD_CFLAGS=$(PKG_CONFIG_PATH="$zpc:${PKG_CONFIG_PATH:-}" pkg-config --cflags libzstd 2>/dev/null)
    ENG_ZSTD_LIBS=$(PKG_CONFIG_PATH="$zpc:${PKG_CONFIG_PATH:-}" pkg-config --libs libzstd 2>/dev/null)
    if [ -n "$ENG_ZSTD_LIBS" ]; then
      ENG_CFLAGS="$ENG_CFLAGS -DCONFIG_ZSTD $ENG_ZSTD_CFLAGS"
      ENG_EXTRA_LIBS="$ENG_EXTRA_LIBS $ENG_ZSTD_LIBS"
    fi
    if [ "$ENG_UNAME_S" = Darwin ]; then
      ENG_EXTRA_LIBS="$ENG_EXTRA_LIBS -lcompression"
    else
      bpc="$(brew --prefix brotli 2>/dev/null)/lib/pkgconfig"
      ENG_BROTLI_CFLAGS=$(PKG_CONFIG_PATH="$bpc:${PKG_CONFIG_PATH:-}" pkg-config --cflags libbrotlienc libbrotlidec 2>/dev/null)
      ENG_BROTLI_LIBS=$(PKG_CONFIG_PATH="$bpc:${PKG_CONFIG_PATH:-}" pkg-config --libs libbrotlienc libbrotlidec 2>/dev/null)
      if [ -n "$ENG_BROTLI_LIBS" ]; then
        ENG_CFLAGS="$ENG_CFLAGS -DCONFIG_BROTLI $ENG_BROTLI_CFLAGS"
        ENG_EXTRA_LIBS="$ENG_EXTRA_LIBS $ENG_BROTLI_LIBS"
      fi
    fi
    [ -n "$(cfg_v ML_NO_SIMD)" ] && ENG_CFLAGS="$ENG_CFLAGS -DDYN_ML_NO_SIMD"
  fi
  # --- CONFIG_HARDEN: probed codegen flags, cached in .obj/.harden-flags ----
  ENG_HARDEN_LDFLAGS=""
  ENG_HARDEN_FORTIFY=""
  if [ "$(cfg_v HARDEN)" = y ]; then eng_harden || return 1; fi
  # --- openlibm (auto-on when the vendored archive exists) -------------------
  ENG_OPENLIBM_DIR="$(cfg_v OPENLIBM_DIR)"; [ -n "$ENG_OPENLIBM_DIR" ] || ENG_OPENLIBM_DIR=third_party/openlibm
  if [ "$(cfg_v OPENLIBM)" = n ]; then
    :
  elif [ -f "$ENG_OPENLIBM_DIR/libopenlibm.a" ]; then
    ENG_CFLAGS="$ENG_CFLAGS -DCONFIG_OPENLIBM -I$ENG_OPENLIBM_DIR/include"
    ENG_OPENLIBM_A="$ROOT/$ENG_OPENLIBM_DIR/libopenlibm.a"
  elif [ -n "$(cfg_v OPENLIBM)" ]; then
    die "CONFIG_OPENLIBM=y but $ENG_OPENLIBM_DIR/libopenlibm.a is missing. Run: \
git clone https://github.com/JuliaMath/openlibm $ENG_OPENLIBM_DIR && build its libopenlibm.a"
  else
    ENG_OPENLIBM_A=""
  fi
  # --- io_uring -------------------------------------------------------------
  if [ -n "$(cfg_v IO_URING)" ]; then
    if [ -n "$(cfg_v TLS)" ]; then
      echo "FAIL: CONFIG_IO_URING=y cannot be combined with CONFIG_TLS=y -- the io_uring backend has no TLS transport (dyn_aio_tls_attach/start exist only in the epoll/kqueue backend); build one or the other" >&2
      exit 1
    fi
    ENG_CFLAGS="$ENG_CFLAGS -DCONFIG_IO_URING"
    ENG_URING_LIB=" -luring"
  else
    ENG_URING_LIB=""
  fi
  # --- libs ------------------------------------------------------------------
  ENG_LIBS="-lm -lpthread"
  [ -z "$(cfg_v WIN32)" ] && ENG_LIBS="$ENG_LIBS -ldl"
  ENG_LIBS="$ENG_LIBS ${ENG_EXTRA_LIBS:-}"
  [ -n "${ENG_OPENLIBM_A-}" ] && ENG_LIBS="$ENG_OPENLIBM_A $ENG_LIBS"
  [ -n "$ENG_URING_LIB" ] && ENG_LIBS="$ENG_LIBS$ENG_URING_LIB"
  ENG_HOST_LIBS="-lm -ldl -lpthread"
  [ -n "${ENG_OPENLIBM_A-}" ] && ENG_HOST_LIBS="$ENG_OPENLIBM_A $ENG_HOST_LIBS"
  [ -n "$ENG_URING_LIB" ] && ENG_HOST_LIBS="$ENG_HOST_LIBS$ENG_URING_LIB"
  # --- final -O variants (fortify rides an -O variable only; LTO splits) -----
  [ -n "$(cfg_v LTO)" ] && eng_lto="-flto" || eng_lto=""
  ENG_CFLAGS_OPT="$ENG_CFLAGS $eng_o"
  ENG_CFLAGS_SMALL="$ENG_CFLAGS -Os"
  ENG_CFLAGS_DEBUG="$ENG_CFLAGS -O0"
  if [ -n "$(cfg_v NATIVE)" ]; then
    ENG_CFLAGS_OPT="$ENG_CFLAGS_OPT $eng_native_flag"
    ENG_CFLAGS_SMALL="$ENG_CFLAGS_SMALL $eng_native_flag"
  fi
  [ -n "$eng_lto" ] && {
    ENG_CFLAGS_OPT="$ENG_CFLAGS_OPT $eng_lto"
    ENG_CFLAGS_SMALL="$ENG_CFLAGS_SMALL $eng_lto"
    ENG_LDFLAGS="$ENG_LDFLAGS $eng_lto"; }
  if [ "$anysan" = 1 ]; then
    fort=$(printf '%s' "${ENG_HARDEN_FORTIFY-}" | tr ' ' '\n' | grep -v '^-D_FORTIFY_SOURCE=' | tr '\n' ' ')
    fort="${fort% }"
  else
    fort="${ENG_HARDEN_FORTIFY-}"
  fi
  [ -n "$fort" ] && {
    ENG_CFLAGS_OPT="$ENG_CFLAGS_OPT $fort"
    ENG_CFLAGS_SMALL="$ENG_CFLAGS_SMALL $fort"; }
  ENG_LDEXPORT=""
  [ -z "$(cfg_v WIN32)" ] && ENG_LDEXPORT="-rdynamic"
  # --- dynajsc's own toolchain (it shells out to CONFIG_CC for -o binaries) --
  if [ -n "$ENG_CROSS_PREFIX" ]; then
    ENG_DYNAJSC="${BIN_PFX}host-dynajsc"
    ENG_DYNAJSC_CC=gcc
  else
    ENG_DYNAJSC="${BIN_PFX}dynajsc$ENG_EXE"
    ENG_DYNAJSC_CC="$ENG_CC"
  fi
  ENG_DYNAJSC_DEFINES="-DCONFIG_CC=\"$ENG_DYNAJSC_CC\" -DCONFIG_PREFIX=\"$ENG_PREFIX\""
  [ -n "$(cfg_v LTO)" ] && ENG_DYNAJSC_DEFINES="$ENG_DYNAJSC_DEFINES -DCONFIG_LTO"
  ENG_LTOEXT=""; [ -n "$(cfg_v LTO)" ] && ENG_LTOEXT=.lto
  # codegen flag sets (verbatim from the Makefile; the duplicated
  # -fno-typedarray in HELLO_OPTS is collapsed to one)
  ENG_HELLO_OPTS="-fno-string-normalize -fno-map -fno-promise -fno-typedarray -fno-regexp -fno-json -fno-eval -fno-proxy -fno-date -fno-module-loader"
  ENG_HELLO_MODULE_OPTS="-fno-string-normalize -fno-map -fno-typedarray -fno-regexp -fno-json -fno-eval -fno-proxy -fno-date -m"
  return 0
}

# (2b) CONFIG_HARDEN probes: every flag is compile-probed with -Werror before it
# is adopted; results cached in $OBJDIR/.harden-flags keyed on the compiler
# version (same cache format as the Makefile: key / flags;... / fortify;...).
# The probes run CONCURRENTLY here (independent compiles); the Makefile ran
# them serially -- same semantics, seconds faster after a clean.
eng_harden(){
  local key cache probes p i tmp ok
  probes="-fstack-protector-strong -fstack-clash-protection -ftrivial-auto-var-init=zero -fno-delete-null-pointer-checks -Wshadow -Wcast-qual -Wpointer-arith -Wvla -Wformat=2 -Wformat-security -Wstrict-prototypes -Wmissing-prototypes -Wdate-time -Wswitch-enum -Wdouble-promotion"
  [ "$ENG_UNAME_M" = x86_64 ] && probes="$probes -fcf-protection=full"
  case "$ENG_UNAME_M" in arm64|aarch64) probes="$probes -mbranch-protection=standard" ;; esac
  [ "$ENG_UNAME_S" = Linux ] && probes="$probes -Wl,-z,relro,-z,now -Wl,-z,noexecstack -Wl,--as-needed"
  key="$("$ENG_CC" --version 2>/dev/null | sed -n 1p | tr -s ' \t' '__')|$ENG_UNAME_S|$ENG_UNAME_M|$(printf '%s' "$probes" | cksum | tr -s ' ' '_')"
  cache="$OBJDIR/.harden-flags"
  if [ -s "$cache" ] && [ "$(sed -n 1p "$cache" 2>/dev/null)" = "$key" ]; then
    flags=$(sed -n 2p "$cache" 2>/dev/null)
    fortline=$(sed -n 3p "$cache" 2>/dev/null)
  else
    tmp=$(mktemp -d "${TMPDIR:-/tmp}/dynaharden.XXXXXX") || return 1
    i=0
    for p in $probes; do
      i=$((i+1))
      ( printf 'int main(void){return 0;}' | $ENG_CC -Werror $p -x c -o /dev/null - >/dev/null 2>&1 \
          && : >"$tmp/$i.ok" ) &
    done
    ( printf 'int main(void){return 0;}' | $ENG_CC -Werror -Werror=cpp -O2 -D_FORTIFY_SOURCE=3 \
        -x c -o /dev/null - >/dev/null 2>&1 && : >"$tmp/fort.ok" ) &
    wait
    flags="flags;"
    i=0
    for p in $probes; do
      i=$((i+1))
      [ -f "$tmp/$i.ok" ] && flags="$flags$p;"
    done
    [ -f "$tmp/fort.ok" ] && fortline="fortify;-D_FORTIFY_SOURCE=3;" || fortline="fortify;"
    rm -rf "$tmp"
    [ -d "$OBJDIR" ] && printf '%s\n%s\n%s\n' "$key" "$flags" "$fortline" > "$cache" 2>/dev/null
  fi
  ENG_HARDEN_FLAGS=$(printf '%s' "${flags#flags;}" | tr ';' ' ')
  ENG_HARDEN_FLAGS="${ENG_HARDEN_FLAGS% }"
  ENG_HARDEN_FORTIFY=$(printf '%s' "${fortline#fortify;}" | tr ';' ' ')
  ENG_HARDEN_FORTIFY="${ENG_HARDEN_FORTIFY% }"
  # -Wl flags are LINK flags: split so they reach LDFLAGS (and never the
  # compile line, where clang calls them unused).
  local hl="" cf=""
  for f in $ENG_HARDEN_FLAGS; do
    case "$f" in -Wl*) hl="$hl $f" ;; *) cf="$cf $f" ;; esac
  done
  ENG_HARDEN_LDFLAGS="${hl# }"
  [ -n "$ENG_HARDEN_LDFLAGS" ] && ENG_LDFLAGS="$ENG_LDFLAGS $ENG_HARDEN_LDFLAGS"
  [ -n "${cf# }" ] && ENG_CFLAGS="$ENG_CFLAGS$cf"
  # The gcc path gets the repo's effective warning policy here too (the clang
  # base already carries it); -funsigned-char deliberately NOT added.
  if [ -z "$(cfg_v CLANG)" ] && [ -z "$(cfg_v COSMO)" ]; then
    ENG_CFLAGS="$ENG_CFLAGS -Wextra -Wno-sign-compare -Wno-missing-field-initializers -Wno-unused-parameter -Wuninitialized -Wunused -Wwrite-strings -Wundef -Wchar-subscripts"
  fi
  return 0
}

# (3) configuration signature: the Makefile's CONFIG_SIG string (same keys, same
# order) plus two cheap extensions (ver/prefix) that close real staleness holes
# the timestamp-only Makefile had. Stored per objdir in .config-sig; a mismatch
# wipes THIS variant's objdir + the shared link outputs (the documented
# config-stamp behavior). The variant stamp (.build-variant, keyed on the
# objdir path) then drops only the shared binaries so each variant keeps its
# object cache.
eng_sig(){
  # src/out are part of the key: the same objdir must never mix objects from a
  # different source tree or artifact root (--src/--out), and a plain rebuild
  # keeps its key only while both match what the cache was built from.
  #
  # ol= records the EFFECTIVE openlibm state, not the CONFIG_ value. The
  # archive lives in the git-ignored third_party/, so the auto-detect flipped
  # on the author's machine without ever appearing in the signature: a tree
  # that gained or lost third_party/openlibm kept its objects and reported
  # "nothing to do (up to date)" while linking against a DIFFERENT libm. That
  # is measurable: lgamma(5) differs by 1 ULP between the two builds, which is
  # exactly enough to fail tests/blackbox/bb_mathx.js's lgamma row.
  local ol_detected="-"
  [ -n "${ENG_OPENLIBM_A:-}" ] && ol_detected="y"
  [ "$(cfg_v OPENLIBM)" = n ] && ol_detected="n"
  ENG_SIG=$(printf 'pur=%s%s%s%s nm=%s tls=%s sq=%s ol=%s mi=%s nat=%s lto=%s pgo=%s%s iur=%s mlno=%s phm=%s op=%s npr=%s pro=%s m32=%s cc=%s cosmo=%s win=%s sl=%s lb=%s ft=%s us=%s hd=%s we=%s ver=%s prefix=%s src=%s out=%s' \
    "$(cfg_v ASAN)" "$(cfg_v MSAN)" "$(cfg_v UBSAN)" "$(cfg_v TSAN)" \
    "$(cfg_v NATIVE_MODULES)" "$(cfg_v TLS)" "$(cfg_v SQLITE)" "$(cfg_v OPENLIBM)" \
    "$ol_detected" "$(cfg_v MIMALLOC)" "$(cfg_v NATIVE)" "$(cfg_v LTO)" \
    "$(cfg_v PGO_GEN)" "$(cfg_v PGO_USE)" "$(cfg_v IO_URING)" "$(cfg_v ML_NO_SIMD)" \
    "$(cfg_v PROP_HASH_MIX)" "$(cfg_v OBJ_POOL)" "$(cfg_v NURSERY_PROBE)" \
    "$(cfg_v PROFILE)" "$(cfg_v M32)" "$(cfg_v CLANG)" "$(cfg_v COSMO)" \
    "$(cfg_v WIN32)" "$(cfg_v SYSTEMLIBM)" "$(cfg_v LITERAL_BOILERPLATE)" \
    "$(cfg_v FAST_PROP_TEARDOWN)" "$(cfg_v USING)" "$(cfg_v HARDEN)" "$(cfg_v WERROR)" \
    "$ENG_VERSION" "$ENG_PREFIX" "$SRC_DIR" "$OUT_BASE")
}
eng_shared_outputs(){ printf '%s' " ${BIN_PFX}dynajs$ENG_EXE ${BIN_PFX}dynajsc$ENG_EXE ${BIN_PFX}run-test262$ENG_EXE ${BIN_PFX}libdynajs.a ${BIN_PFX}libdynajs.lto.a ${BIN_PFX}libdynajs.fuzz.a ${BIN_PFX}host-dynajsc"; }
eng_ensure(){
  eng_sig
  local cur
  cur=$(cat "$OBJDIR/.config-sig" 2>/dev/null || true)
  if [ "$cur" != "$ENG_SIG" ]; then
    # shellcheck disable=SC2086
    rm -rf "$OBJDIR" >/dev/null 2>&1
    eval "rm -f $(eng_shared_outputs)" >/dev/null 2>&1
    mkdir -p "$OBJDIR"
    printf '%s' "$ENG_SIG" > "$OBJDIR/.config-sig"
    eng_progress "config sig changed -> wiped $OBJDIR + shared outputs"
  fi
  mkdir -p "$OUT_BASE"
  # dynajsc self-locates its headers + archive beside argv[0] (exe_dir/src/
  # dynajs.h, else the install prefix). With --out the binary lives in
  # OUT_BASE, so give OUT_BASE the view dynajsc expects: src/ -> the source
  # tree, libdynajs.a already archived right there.
  if [ "$BIN_PFX" != "./" ]; then
    case "$SRC_DIR" in
      /*) src_abs=$SRC_DIR ;;
      *)  src_abs=$ROOT/$SRC_DIR ;;
    esac
    ln -sfn "$src_abs" "$OUT_BASE/src"
  fi
  local want="objdir=$OBJDIR"
  cur=$(cat "$STAMP_FILE" 2>/dev/null || true)
  if [ "$cur" != "$want" ]; then
    eval "rm -f $(eng_shared_outputs)" >/dev/null 2>&1
    printf '%s' "$want" > "$STAMP_FILE"
    eng_progress "variant changed -> $want (dropped shared binaries, kept objects)"
  fi
  mkdir -p "$OBJDIR" "$OBJDIR/examples" "$OBJDIR/tests"
  printf '%s' "${ENG_CFG_DESC:-"(defaults)"}" > "$OBJDIR/.dev_cfg" 2>/dev/null || true
}

# (4) object inventory -- the exact Makefile lists, as basenames.
eng_objects(){
  ENG_CORE_OBJS="dyn-hash dyn-codec dyn-prng dyn-compress dyn-ds dyn-serial dyn-path dyn-mathx dyn-ac dyn-dict dyn-pool dyn-timer dyn-dns dyn-resp dyn-scram dyn-snappy"
  ENG_LIB_OBJS="dynajs engine-parser engine-serialize dtoa libregexp libunicode cutils dyna-libc dyna-io $ENG_CORE_OBJS dyna-simd-core dyna-simd-scalar dyna-simd-neon dyna-simd-sse42 dyna-simd-avx2 dyna-simd-avx512 dyna-simd-sve"
  [ -n "$(cfg_v MIMALLOC)" ] && ENG_LIB_OBJS="$ENG_LIB_OBJS mimalloc"
  ENG_NAT_OBJS=""
  if [ -n "$(cfg_v NATIVE_MODULES)" ]; then
    ENG_NAT_OBJS="dyna-nat dyna-aio dyna-aio-uring dyna-evloop"
    local m
    for m in random compress stream async; do
      [ -f "$SRC_DIR/dyna-$m.c" ] && ENG_NAT_OBJS="$ENG_NAT_OBJS dyna-$m"
    done
    if [ -f "$SRC_DIR/dyna-net.c" ]; then
      [ -n "$(cfg_v TLS)" ] && ENG_NAT_OBJS="$ENG_NAT_OBJS dyna-tls"
      ENG_NAT_OBJS="$ENG_NAT_OBJS dyna-net dyna-http dyna-netip dyna-net-tcp dyna-net-proxy dyna-net-dns dyna-net-redis dyna-net-pg dyna-net-ratelimit dyna-net-metrics"
      [ -n "$ENG_SQLITE_LIBS" ] && ENG_NAT_OBJS="$ENG_NAT_OBJS dyna-net-sqlite"
    fi
    if [ -f "$SRC_DIR/dyna-structures.c" ]; then
      ENG_NAT_OBJS="$ENG_NAT_OBJS dyna-structures dyna-serialize dyna-graph"
    fi
    for m in structures3 ml simd file semver bytes crypto matcher encoding time \
             mathx csv dataframe uuid config log url term validate json schema \
             xml yaml decimal vserialize protobuf asn1 html oauth2 bench; do
      [ -f "$SRC_DIR/dyna-$m.c" ] && ENG_NAT_OBJS="$ENG_NAT_OBJS dyna-$m"
    done
    if [ -f "$SRC_DIR/dyna-sys.c" ]; then
      ENG_NAT_OBJS="$ENG_NAT_OBJS dyna-sys"
      [ -f "$SRC_DIR/dyna-scrape.c" ] && ENG_NAT_OBJS="$ENG_NAT_OBJS dyna-scrape"
    fi
    if [ -n "$(cfg_v IO_URING)" ] && [ -f "$SRC_DIR/dyna-uring.c" ]; then
      ENG_NAT_OBJS="$ENG_NAT_OBJS dyna-uring"
    fi
  fi
  # examples build? (no cross, and none of asan/msan/ubsan -- exactly the
  # Makefile's PROGS condition; tsan still gets examples there, so do we)
  ENG_BUILD_EXAMPLES=0
  if [ -z "$ENG_CROSS_PREFIX" ] && [ -z "$(cfg_v ASAN)" ] && [ -z "$(cfg_v MSAN)" ] && [ -z "$(cfg_v UBSAN)" ]; then
    ENG_BUILD_EXAMPLES=1
  fi
  # ENG_NO_EXAMPLES: callers that reproduce a narrower legacy goal (the pgo
  # flow built only the dynajs target) skip the example binaries; a dynajsc
  # codegen link cannot carry the profile runtime, so an instrumented tree
  # must not link them
  [ -n "${ENG_NO_EXAMPLES:-}" ] && ENG_BUILD_EXAMPLES=0
  ENG_BUILD_SHARED=0
  [ -n "$(cfg_v SHARED_LIBS)" ] && [ "$ENG_BUILD_EXAMPLES" = 1 ] && ENG_BUILD_SHARED=1
}

# source resolution (the VPATH src:src/core:tools port + generated/explicit
# roots; the src legs honor --src). $1 = object basename (may be "examples/fib"),
# prints the source path.
eng_src_of(){
  case "$1" in
    repl)          printf '%s' "${GEN_PFX}repl.c" ; return ;;
    hello)         printf '%s' "${GEN_PFX}hello.c" ; return ;;
    test_fib)      printf '%s' "${GEN_PFX}test_fib.c" ; return ;;
    mimalloc)      printf '%s' "third_party/mimalloc/src/static.c" ; return ;;
    examples/fib)  printf '%s' "examples/fib.c" ; return ;;
    examples/point) printf '%s' "examples/point.c" ; return ;;
  esac
  [ -f "$SRC_DIR/$1.c" ]      && { printf '%s' "$SRC_DIR/$1.c"; return; }
  [ -f "$SRC_DIR/core/$1.c" ] && { printf '%s' "$SRC_DIR/core/$1.c"; return; }
  [ -f "tools/$1.c" ]    && { printf '%s' "tools/$1.c"; return; }
  [ -f "$1.c" ]          && { printf '%s' "$1.c"; return; }
  return 1
}

# dependency-tracked staleness: source newer, OR any header listed in the
# object's .d file newer, OR no .d (conservative), OR object missing.
eng_deps_of(){
  local raw
  raw=$(tr '\\\n' '  ' < "$1" 2>/dev/null) || return 1
  raw=${raw#*:}
  printf '%s' "$raw"
}
eng_stale(){
  local obj="$1" src="$2" d deps dep
  [ -f "$obj" ] || return 0
  [ "$src" -nt "$obj" ] && return 0
  d="$obj.d"
  [ -f "$d" ] || return 0
  deps=$(eng_deps_of "$d") || return 0
  for dep in $deps; do
    [ -f "$dep" ] || continue
    [ "$dep" -nt "$obj" ] && return 0
  done
  return 1
}

# (5) the parallel compile pool. stdin: TAB-separated src/obj/mode/extra specs.
# mode: opt|check|pic|host|nolto|mimalloc. Failures are logged per object and
# reported together; the pool fails on the first failure found.
ENG_CC_ONE(){
  local src="$1" obj="$2" mode="$3" extra="$4" cf
  case "$mode" in
    opt)      cf=$ENG_CFLAGS_OPT ;;
    check)    cf=$ENG_CFLAGS ;;
    pic)      cf=$ENG_CFLAGS_OPT ;;
    nolto)    cf=$ENG_CFLAGS_NOLTO ;;
    debug)    cf=$ENG_CFLAGS_DEBUG ;;
    host)     cf=$ENG_CFLAGS_OPT ;;
    mimalloc) cf="" ;;
    fuzz)     cf="$ENG_CFLAGS_OPT ${FUZZ_SAN_FLAGS:-}" ;;
    *)        cf=$ENG_CFLAGS_OPT ;;
  esac
  mkdir -p "$(dirname "$obj")" || return 1
  case "$mode" in
    mimalloc)
      $ENG_CC -O2 -DNDEBUG -Ithird_party/mimalloc/include -c -o "$obj" "$src" ;;
    host)
      $ENG_HOST_CC $cf $extra -MMD -MF "$obj.d" -c -o "$obj" "$src" ;;
    pic)
      $ENG_CC $cf -fPIC -DJS_SHARED_LIBRARY $extra -MMD -MF "$obj.d" -c -o "$obj" "$src" ;;
    check)
      $ENG_CC $cf $extra -DCONFIG_CHECK_JSVALUE -MMD -MF "$obj.d" -c -o "$obj" "$src" ;;
    fuzz)
      $ENG_CC $cf -fsanitize=fuzzer-no-link $extra -MMD -MF "$obj.d" -c -o "$obj" "$src" ;;
    *)
      # native-module and core sources allocate through the tracked wrappers
      # (src/dyna-track.h, bodies at the end of dyna-io.c) so
      # --native-memory-limit sees them; dyna-nat.c keeps its own exact-size
      # header allocator and must reach the real one. The wrappers are a
      # flag test until a limit is first set (measured: probing the usable
      # size on every free cost +14% on `new URL`, +12% on a Robots build)
      case "$cf:${src##*/}" in
        *-DCONFIG_NATIVE_MODULES*:dyna-nat.c) ;;
        *-DCONFIG_NATIVE_MODULES*:dyna-aio.c|*-DCONFIG_NATIVE_MODULES*:dyna-aio-uring.c|*-DCONFIG_NATIVE_MODULES*:dyna-evloop.c|*-DCONFIG_NATIVE_MODULES*:dyn-pool.c|*-DCONFIG_NATIVE_MODULES*:dyn-timer.c)
          # reactor / pool / timer bookkeeping is counted but never refused:
          # a completion dropped on a failed allocation strands a promise
          extra="$extra -DDYN_TRK_PLUMBING -include $SRC_DIR/dyna-track.h" ;;
        *-DCONFIG_NATIVE_MODULES*:dyna-*.c|*-DCONFIG_NATIVE_MODULES*:dyn-*.c)
          extra="$extra -include $SRC_DIR/dyna-track.h" ;;
      esac
      $ENG_CC $cf $extra -MMD -MF "$obj.d" -c -o "$obj" "$src" ;;
  esac
}
# specs: a string of TAB-separated "src obj mode extra" lines
eng_pool(){
  local specs="$1" total=0 n=0 fail=0 src obj mode extra log rclog base rc
  # ENG_POOL_LOGDIR: a caller-owned log dir (the fuzz pool uses one so its
  # warnings can never feed the engine build's 0-warning sweep)
  local logdir="${ENG_POOL_LOGDIR:-$OBJDIR/.build-logs}"
  while IFS=$'\t' read -r src obj mode extra; do
    [ -n "${src:-}" ] && total=$((total+1))
  done <<< "$specs"
  [ "$total" -eq 0 ] && return 0
  ENG_DID_WORK=1
  rm -rf "$logdir"; mkdir -p "$logdir"
  eng_progress "compile pool: $total objects, -j$JOBS"
  while IFS=$'\t' read -r src obj mode extra; do
    [ -n "${src:-}" ] || continue
    n=$((n+1))
    while [ "$(jobs -rp | wc -l | tr -d ' ')" -ge "$JOBS" ]; do
      wait -n 2>/dev/null || sleep 0.05
    done
    printf '  [%d/%d] CC %s\n' "$n" "$total" "${obj#$OBJDIR/}"
    base=$(basename "$obj")
    log="$logdir/$base.log"
    { ENG_CC_ONE "$src" "$obj" "${mode:-opt}" "$extra" >"$log" 2>&1; echo $? > "$log.rc"; } &
  done <<< "$specs"
  wait
  for rclog in "$logdir"/*.rc; do
    [ -e "$rclog" ] || continue
    rc=$(cat "$rclog")
    [ "$rc" = 0 ] && continue
    fail=$((fail+1))
    log="${rclog%.rc}"
    base=$(basename "$log" .log)
    echo "FAIL: compile $base (rc=$rc)"
    sed 's/^/    /' "$log" 2>/dev/null | tail -25
    echo "    full log: $log"
  done
  [ "$fail" -eq 0 ] && return 0
  echo "FAIL: build [$fail of $total objects failed]"
  return 1
}

# link/codegen helpers --------------------------------------------------------
ENG_BG_RCS=()
eng_bg(){ # run a command in the background, logging + recording its rc file
  local log="$1"; shift
  ENG_DID_WORK=1
  { "$@" >"$log" 2>&1; echo $? > "$log.rc"; } &
  ENG_BG_RCS+=("$log.rc")
}
eng_bg_reap(){ # wait for every background stage; fail on any nonzero rc
  local f rc fail=0
  wait
  [ "${#ENG_BG_RCS[@]}" -eq 0 ] && return 0
  for f in ${ENG_BG_RCS[@]}; do
    [ -e "$f" ] || { echo "FAIL: background stage wrote no rc: $f"; fail=1; continue; }
    rc=$(cat "$f")
    [ "$rc" = 0 ] && continue
    fail=1
    echo "FAIL: link/codegen stage rc=$rc -- ${f%.rc}"
    sed 's/^/    /' "${f%.rc}.log" 2>/dev/null | tail -25
  done
  ENG_BG_RCS=()
  return "$fail"
}
# link stages log under .link-logs so the compile pool can wipe .build-logs
# without destroying their rc files.
eng_link_logs(){ mkdir -p "$OBJDIR/.link-logs"; printf '%s' "$OBJDIR/.link-logs"; }
eng_out_stale(){
  local out="$1"; shift
  [ -f "$out" ] || return 0
  local m
  for m in "$@"; do
    [ "$m" -nt "$out" ] && return 0
  done
  return 1
}
eng_link_dynajs(){
  local out="${BIN_PFX}dynajs$ENG_EXE"
  # shellcheck disable=SC2086
  $ENG_CC $ENG_LDFLAGS $ENG_LDEXPORT -o "$out" \
    $(for b in dyna-cli repl $ENG_LIB_OBJS $ENG_NAT_OBJS; do printf '%s ' "$OBJDIR/$b.o"; done) \
    $ENG_LIBS || return 1
  [ "$OBJDIR" != "$OUT_BASE" ] && { mkdir -p "$OBJDIR"; cp -f "$out" "$OBJDIR/$(basename "$out")" 2>/dev/null || true; }
  echo "  LINK $out"
}
eng_link_dynajsc(){
  local out="${BIN_PFX}dynajsc$ENG_EXE"
  # shellcheck disable=SC2086
  $ENG_CC $ENG_LDFLAGS -o "$out" "$OBJDIR/dynajsc.o" \
    $(for b in $ENG_LIB_OBJS; do printf '%s ' "$OBJDIR/$b.o"; done) \
    $ENG_LIBS || return 1
  [ "$OBJDIR" != "$OUT_BASE" ] && { mkdir -p "$OBJDIR"; cp -f "$out" "$OBJDIR/$(basename "$out")" 2>/dev/null || true; }
  echo "  LINK $out"
}
eng_link_t262(){
  local out="${BIN_PFX}run-test262$ENG_EXE"
  # shellcheck disable=SC2086
  $ENG_CC $ENG_LDFLAGS -o "$out" "$OBJDIR/run-test262.o" \
    $(for b in $ENG_LIB_OBJS; do printf '%s ' "$OBJDIR/$b.o"; done) \
    $ENG_LIBS || return 1
  echo "  LINK $out"
}
eng_archive(){ # $1 = archive, rest = members
  local out="$1"; shift
  "$ENG_AR" rcs "$out" "$@" || return 1
  echo "  AR $out"
}

# the full build: config -> wipe/stamp -> batch1 compiles -> dynajsc link ->
# codegen -> batch2 compiles -> dynajs/archive/examples links.
ENG_NCOMPILED=0
ENG_DID_WORK=0
engine_build(){
  local t0 rc
  t0=$(_now)
  eng_parse_cfg "$@" || return 1
  eng_detect
  if [ "${ENG_SKIP_LOCK:-0}" != 1 ]; then
    eng_lock_acquire || return 1
  fi
  eng_progress "engine build start: cfg=[${ENG_CFG_DESC:-"(defaults)"}] objdir-args=[$*] jobs=$JOBS"
  eng_ensure || { eng_lock_release; return 1; }
  eng_flags   || { eng_lock_release; return 1; }
  eng_objects
  if [ "$DEBUG" = 1 ]; then
    echo "[devsh] engine: CC=$ENG_CC AR=$ENG_AR OBJDIR=$OBJDIR" >&2
    echo "[devsh] engine: CFLAGS_OPT=$ENG_CFLAGS_OPT" >&2
    echo "[devsh] engine: LDFLAGS=$ENG_LDFLAGS LIBS=$ENG_LIBS" >&2
  fi

  # ---- compile batch 1: everything except objects of generated C files -----
  # each spec passes the dependency-tracked staleness gate first: source newer,
  # or any header in the object's .d newer, or the .d/object missing.
  local specs="" b src obj n=0
  src=$(eng_src_of dynajsc) || { die "build: dynajsc.c not found in the source tree (src dir: $SRC_DIR)"; return 1; }
  obj="$OBJDIR/dynajsc.o"
  eng_stale "$obj" "$src" && { specs+="$src"$'\t'"$obj"$'\t'"opt"$'\t'"$ENG_DYNAJSC_DEFINES"$'\n'; n=$((n+1)); }
  src=$(eng_src_of dyna-cli) || { die "build: dyna-cli.c not found in the source tree (src dir: $SRC_DIR)"; return 1; }
  obj="$OBJDIR/dyna-cli.o"
  eng_stale "$obj" "$src" && { specs+="$src"$'\t'"$obj"$'\t'"opt"$'\t'$'\n'; n=$((n+1)); }
  for b in $ENG_LIB_OBJS; do
    if [ "$b" = mimalloc ]; then
      src=$(eng_src_of mimalloc); obj="$OBJDIR/mimalloc.o"
      eng_stale "$obj" "$src" && { specs+="$src"$'\t'"$obj"$'\t'"mimalloc"$'\t'$'\n'; n=$((n+1)); }
      continue
    fi
    src=$(eng_src_of "$b") || { die "build: no source for $b.c"; return 1; }
    obj="$OBJDIR/$b.o"
    eng_stale "$obj" "$src" && { specs+="$src"$'\t'"$obj"$'\t'"opt"$'\t'$'\n'; n=$((n+1)); }
  done
  for b in $ENG_NAT_OBJS; do
    src=$(eng_src_of "$b") || { die "build: no source for $b.c"; return 1; }
    obj="$OBJDIR/$b.o"
    eng_stale "$obj" "$src" && { specs+="$src"$'\t'"$obj"$'\t'"opt"$'\t'$'\n'; n=$((n+1)); }
  done
  # unicode table bootstrap (only when the UnicodeData source is vendored)
  if [ -f unicode/UnicodeData.txt ]; then
    eng_unicode_bootstrap || { eng_lock_release; return 1; }
  fi
  src=$(eng_src_of run-test262) || { die "build: tools/run-test262.c missing"; return 1; }
  obj="$OBJDIR/run-test262.o"
  eng_stale "$obj" "$src" && { specs+="$src"$'\t'"$obj"$'\t'"opt"$'\t'$'\n'; n=$((n+1)); }
  eng_pool "$specs" || { eng_lock_release; return 1; }
  ENG_NCOMPILED=$n

  # ---- dynajsc link (foreground wait), then t262 + archive in background ---
  local llog; llog=$(eng_link_logs)
  rm -f "$llog"/link-*.log "$llog"/link-*.rc 2>/dev/null
  ENG_BG_RCS=()
  local arch_members="$(for b in $ENG_LIB_OBJS; do printf '%s ' "$OBJDIR/$b.o"; done)"
  if eng_out_stale "${BIN_PFX}dynajsc$ENG_EXE" "$OBJDIR/dynajsc.o" $arch_members; then
    echo "  LINK dynajsc$ENG_EXE"
    eng_bg "$llog/link-dynajsc.log" eng_link_dynajsc
    local dj_pid=$!
    wait "$dj_pid"
    if [ "$(cat "$llog/link-dynajsc.log.rc" 2>/dev/null || echo 1)" != 0 ]; then
      eng_bg_reap; eng_lock_release; return 1
    fi
  fi
  if eng_out_stale "${BIN_PFX}run-test262$ENG_EXE" "$OBJDIR/run-test262.o" $arch_members; then
    echo "  LINK run-test262$ENG_EXE"
    eng_bg "$llog/link-t262.log" eng_link_t262
  fi
  if [ -z "$(cfg_v LTO)" ]; then
    if eng_out_stale "${BIN_PFX}libdynajs.a" $arch_members; then
      echo "  AR libdynajs.a"
      eng_bg "$llog/link-ar.log" eng_archive "${BIN_PFX}libdynajs.a" $arch_members
    fi
  else
    # LTO: libdynajs.lto.a from the LTO objects, libdynajs.a from a parallel
    # non-LTO object set (CFLAGS_NOLTO, which never saw the post-OPT appends).
    local nspecs="" nsrc nmembers="" n=0
    for b in $ENG_LIB_OBJS; do
      nsrc=$(eng_src_of "$b") || { die "build: no source for $b.c"; return 1; }
      if [ "$b" = mimalloc ]; then
        obj="$OBJDIR/mimalloc.nolto.o"
      else
        obj="$OBJDIR/$b.nolto.o"
      fi
      nmembers+="$obj "
      eng_stale "$obj" "$nsrc" && { nspecs+="$nsrc"$'\t'"$obj"$'\t'"nolto"$'\t'$'\n'; n=$((n+1)); }
    done
    if eng_out_stale "${BIN_PFX}libdynajs.lto.a" $arch_members; then
      echo "  AR libdynajs.lto.a"
      eng_bg "$llog/link-ar-lto.log" eng_archive "${BIN_PFX}libdynajs.lto.a" $arch_members
    fi
    eng_pool "$nspecs" || { eng_lock_release; return 1; }
    ENG_NCOMPILED=$((ENG_NCOMPILED + n))
    if eng_out_stale "${BIN_PFX}libdynajs.a" $nmembers; then
      echo "  AR libdynajs.a (nolto)"
      eng_bg "$llog/link-ar.log" eng_archive "${BIN_PFX}libdynajs.a" $nmembers
    fi
  fi

  # ---- codegen: repl.c (always), hello.c / test_fib.c (examples builds) ----
  eng_codegen || { eng_bg_reap; eng_lock_release; return 1; }

  # ---- compile batch 2: objects of generated C files -----------------------
  local specs2=""
  if eng_stale "$OBJDIR/repl.o" "${GEN_PFX}repl.c"; then
    specs2+="${GEN_PFX}repl.c"$'\t'"$OBJDIR/repl.o"$'\t'"opt"$'\t'$'\n'
  fi
  if [ "$ENG_BUILD_EXAMPLES" = 1 ]; then
    eng_stale "$OBJDIR/hello.o" "${GEN_PFX}hello.c" && specs2+="${GEN_PFX}hello.c"$'\t'"$OBJDIR/hello.o"$'\t'"opt"$'\t'$'\n'
    eng_stale "$OBJDIR/test_fib.o" "${GEN_PFX}test_fib.c" && specs2+="${GEN_PFX}test_fib.c"$'\t'"$OBJDIR/test_fib.o"$'\t'"opt"$'\t'$'\n'
    eng_stale "$OBJDIR/examples/fib.o" examples/fib.c && specs2+="examples/fib.c"$'\t'"$OBJDIR/examples/fib.o"$'\t'"opt"$'\t'$'\n'
    if [ "$ENG_BUILD_SHARED" = 1 ]; then
      eng_stale "$OBJDIR/examples/fib.pic.o" examples/fib.c && specs2+="examples/fib.c"$'\t'"$OBJDIR/examples/fib.pic.o"$'\t'"pic"$'\t'$'\n'
      eng_stale "$OBJDIR/examples/point.pic.o" examples/point.c && specs2+="examples/point.c"$'\t'"$OBJDIR/examples/point.pic.o"$'\t'"pic"$'\t'$'\n'
    fi
  fi
  eng_pool "$specs2" || { eng_bg_reap; eng_lock_release; return 1; }
  ENG_NCOMPILED=$((ENG_NCOMPILED + $(printf '%s\n' "$specs2" | grep -c .)))

  # ---- final links ----------------------------------------------------------
  eng_bg_reap || { eng_lock_release; return 1; }

  local jobs_bg=0
  if eng_out_stale "${BIN_PFX}dynajs$ENG_EXE" "$OBJDIR/dyna-cli.o" "$OBJDIR/repl.o" \
      $(for b in $ENG_LIB_OBJS $ENG_NAT_OBJS; do printf '%s ' "$OBJDIR/$b.o"; done); then
    echo "  LINK dynajs$ENG_EXE"
    eng_bg "$llog/link-dynajs.log" eng_link_dynajs
    jobs_bg=1
  fi
  if [ "$ENG_BUILD_EXAMPLES" = 1 ]; then
    [ "$BIN_PFX" = "./" ] || mkdir -p "${BIN_PFX}examples"
    local hello_lib="${BIN_PFX}libdynajs$ENG_LTOEXT.a"
    if eng_out_stale "${BIN_PFX}examples/hello" "$OBJDIR/hello.o" $arch_members; then
      echo "  LINK examples/hello"
      eng_bg "$llog/link-hello.log" $ENG_CC $ENG_LDFLAGS -o "${BIN_PFX}examples/hello" \
        "$OBJDIR/hello.o" $arch_members $ENG_LIBS
      jobs_bg=1
    fi
    if eng_out_stale "${BIN_PFX}examples/test_fib" "$OBJDIR/test_fib.o" "$OBJDIR/examples/fib.o" "$hello_lib"; then
      echo "  LINK examples/test_fib"
      eng_bg "$llog/link-testfib.log" $ENG_CC $ENG_LDFLAGS -o "${BIN_PFX}examples/test_fib" \
        "$OBJDIR/test_fib.o" "$OBJDIR/examples/fib.o" "$hello_lib" $ENG_LIBS
      jobs_bg=1
    fi
    if [ -z "$(cfg_v M32)" ] && [ -z "$(cfg_v WIN32)" ]; then
      if eng_out_stale "${BIN_PFX}examples/hello_module" "$ENG_DYNAJSC" "$hello_lib" examples/hello_module.js; then
        echo "  GEN examples/hello_module"
        eng_bg "$llog/link-hellomod.log" "$ENG_DYNAJSC" $ENG_HELLO_MODULE_OPTS -o "${BIN_PFX}examples/hello_module" examples/hello_module.js
        jobs_bg=1
      fi
    fi
  fi
  if [ "$ENG_BUILD_SHARED" = 1 ]; then
    if eng_out_stale "${BIN_PFX}examples/fib.so" "$OBJDIR/examples/fib.pic.o"; then
      echo "  LINK examples/fib.so"
      eng_bg "$llog/link-fibso.log" $ENG_CC $ENG_LDFLAGS -shared -o "${BIN_PFX}examples/fib.so" "$OBJDIR/examples/fib.pic.o"
      jobs_bg=1
    fi
    if eng_out_stale "${BIN_PFX}examples/point.so" "$OBJDIR/examples/point.pic.o"; then
      echo "  LINK examples/point.so"
      eng_bg "$llog/link-pointso.log" $ENG_CC $ENG_LDFLAGS -shared -o "${BIN_PFX}examples/point.so" "$OBJDIR/examples/point.pic.o"
      jobs_bg=1
    fi
  fi
  if [ "$jobs_bg" = 1 ]; then
    eng_bg_reap || { eng_lock_release; return 1; }
  else
    wait 2>/dev/null
    ENG_BG_RCS=()
  fi

  eng_strict_warnings || { eng_lock_release; return 1; }
  eng_lock_release
  local t1 dt
  t1=$(_now); dt=$(_elapsed "$t0" "$t1")
  eng_progress "engine build done in ${dt}s (cfg=[${ENG_CFG_DESC:-"(defaults)"}])"
  if [ "${ENG_DID_WORK:-0}" = 1 ]; then
    echo "build: ok in ${dt}s ($ENG_NCOMPILED objects compiled, objdir $OBJDIR)"
  else
    echo "build: ok -- nothing to do (up to date; objdir $OBJDIR)"
  fi
  return 0
}

# --- the build lock ---------------------------------------------------------
# One lock per TREE (and per objdir inside it), never a fixed world-shared
# /tmp path: two checkouts must never serialize against each other, and two
# builds in ONE checkout must never write one objdir concurrently (B1-07,
# B1-14). The lock directory holds an owner record (pid, host, start, command)
# so a stale lock left by a SIGKILLed build is detected instead of blocking
# every later build forever, and the wait is bounded and noisy: a timeout
# fails the build naming the lock and its owner, it never hangs silently.
#
# Lock path:  $DEV_STATE/locks/<objdir-key>.lock      (DEV_STATE: $ROOT/.dev)
# Env:        DEV_LOCK_TIMEOUT  seconds to wait before failing (default 900)
#             DEV_LOCK_STALE    age at which a live-looking lock is broken
#                               anyway (default 3600; 0 disables)
#             DEV_LOCK_DIR      override the whole lock directory
ENG_LOCK_DIR=""
ENG_LOCK_HELD=""
_eng_lock_root(){
  printf '%s' "${DEV_LOCK_DIR:-$DEV_STATE/locks}"
}
_eng_lock_key(){ printf '%s' "${OBJDIR:-.obj}" | tr -c 'A-Za-z0-9._-' '_'; }
_eng_lock_alive(){ # pid -> 0 when the process exists and is ours to signal
  local p="$1" host
  [ -n "$p" ] || return 1
  host=$(hostname 2>/dev/null || echo unknown)
  [ "$(sed -n 's/^host=//p' "$ENG_LOCK_DIR/owner" 2>/dev/null)" = "$host" ] || return 0
  kill -0 "$p" 2>/dev/null
}
_eng_lock_read_owner(){
  sed -n 's/^[a-z]*=//p' "$ENG_LOCK_DIR/owner" 2>/dev/null | tr '\n' ' '
}
eng_lock_acquire(){
  local waited=0 tmo stale now start owner pid
  ENG_LOCK_DIR="$(_eng_lock_root)/$(_eng_lock_key).lock"
  mkdir -p "$(_eng_lock_root)" 2>/dev/null || die "build: cannot create the lock directory $(_eng_lock_root)"
  tmo=${DEV_LOCK_TIMEOUT:-900}
  stale=${DEV_LOCK_STALE:-3600}
  while :; do
    if mkdir "$ENG_LOCK_DIR" 2>/dev/null; then
      printf 'pid=%s\nhost=%s\nstart=%s\ncmd=%s\nobjdir=%s\n' \
        "$$" "$(hostname 2>/dev/null || echo unknown)" "$(_now)" \
        "${cmd:-build.sh} $*" "$OBJDIR" > "$ENG_LOCK_DIR/owner" 2>/dev/null
      ENG_LOCK_HELD=1
      return 0
    fi
    owner=$(_eng_lock_read_owner)
    pid=$(sed -n 's/^pid=//p' "$ENG_LOCK_DIR/owner" 2>/dev/null)
    start=$(sed -n 's/^start=//p' "$ENG_LOCK_DIR/owner" 2>/dev/null)
    now=$(_now)
    # a lock whose owner is gone (SIGKILL, power loss, a killed parent shell)
    # is stale: reclaim it and say so.
    if [ -n "$pid" ] && ! _eng_lock_alive "$pid"; then
      echo "build: reclaiming stale build lock $ENG_LOCK_DIR (owner pid $pid is gone)" >&2
      rm -rf "$ENG_LOCK_DIR" 2>/dev/null
      continue
    fi
    # an ownerless lock (a crash between mkdir and the owner write) or one
    # older than DEV_LOCK_STALE is broken rather than waited on forever.
    local age=0
    [ -n "$start" ] && age=$(( ${now%%.*} - ${start%%.*} ))
    if [ -z "$owner" ] || { [ "$stale" -gt 0 ] && [ "$age" -gt "$stale" ]; }; then
      echo "build: breaking build lock $ENG_LOCK_DIR (owner ${owner:-unknown}, age ${age}s)" >&2
      rm -rf "$ENG_LOCK_DIR" 2>/dev/null
      continue
    fi
    waited=$((waited + 1))
    if [ $((waited % 15)) -eq 1 ] || [ "$waited" = 1 ]; then
      echo "build: waiting ${waited}s for the build lock $ENG_LOCK_DIR" >&2
      echo "       held by: ${owner:-unknown}" >&2
    fi
    if [ "$waited" -ge "$tmo" ]; then
      echo "FAIL: build lock $ENG_LOCK_DIR held for ${waited}s by: ${owner:-unknown}" >&2
      echo "      nothing else in THIS tree is building, so either the owner is wedged" >&2
      echo "      (see 'ps -p <pid>') or the lock is abandoned:" >&2
      echo "          rm -rf '$ENG_LOCK_DIR'" >&2
      echo "      raise the wait with DEV_LOCK_TIMEOUT=<seconds> if a real build is slow." >&2
      return 1
    fi
    sleep 1
  done
}
eng_lock_release(){
  if [ -n "${ENG_LOCK_HELD:-}" ]; then
    rmdir "$ENG_LOCK_DIR" 2>/dev/null || rm -rf "$ENG_LOCK_DIR" 2>/dev/null
    ENG_LOCK_HELD=""
  fi
}
trap 'eng_lock_release' EXIT

# --- the tree lock ----------------------------------------------------------
# The build lock above serialises COMPILES into one objdir. It does not stop a
# test run from starting while a build is replacing the binary under it, or a
# second gate stage from reconfiguring the tree a first one is measuring -- and
# both produce failures that look like bugs in a file nobody touched. So every
# subcommand that writes this tree's objects or runs its binary holds ONE lock
# for its whole life, and a second such command FAILS AT ONCE naming the owner;
# it never queues behind it (DEV_TREE_WAIT=<seconds> opts into waiting).
# Children of the owner (the gate's stages, a recipe that re-enters build.sh)
# inherit DYN_TREE_LOCK and pass straight through. The read-only subcommands
# in TREE_LOCK_FREE never take it.
TREE_LOCK_DIR=""
TREE_LOCK_HELD=""
TREE_LOCK_FREE=" help -h --help status modules-changed slim-vars ws stats sbom install-hooks check-anchors check-error-ids check-types check-dts-truth check-test-list check-hooks conformance __cc-lines __cc-line api-inventory "
tree_lock_acquire(){
  case "$TREE_LOCK_FREE" in *" ${cmd:-help} "*) return 0 ;; esac
  [ -n "${DYN_TREE_LOCK:-}" ] && return 0
  local waited=0 tmo=${DEV_TREE_WAIT:-0} pid owner
  TREE_LOCK_DIR="$(_eng_lock_root)/tree.lock"
  mkdir -p "$(_eng_lock_root)" 2>/dev/null || die "cannot create the lock directory $(_eng_lock_root)"
  while :; do
    if mkdir "$TREE_LOCK_DIR" 2>/dev/null; then
      printf 'pid=%s\nhost=%s\nstart=%s\ncmd=%s\n' "$$" "$(hostname 2>/dev/null || echo unknown)" \
        "$(_now)" "build.sh ${cmd:-} $*" > "$TREE_LOCK_DIR/owner" 2>/dev/null
      TREE_LOCK_HELD=1
      export DYN_TREE_LOCK=$$
      return 0
    fi
    pid=$(sed -n 's/^pid=//p' "$TREE_LOCK_DIR/owner" 2>/dev/null)
    owner=$(sed -n 's/^[a-z]*=//p' "$TREE_LOCK_DIR/owner" 2>/dev/null | tr '\n' ' ')
    if [ -n "$pid" ] && ! kill -0 "$pid" 2>/dev/null; then
      echo "build.sh: reclaiming the tree lock (owner pid $pid is gone)" >&2
      rm -rf "$TREE_LOCK_DIR" 2>/dev/null
      continue
    fi
    if [ -z "$pid" ] && [ "$waited" -ge 2 ]; then
      rm -rf "$TREE_LOCK_DIR" 2>/dev/null
      continue
    fi
    if [ "$waited" -ge "$tmo" ] && [ -n "$pid" ]; then
      echo "FAIL: another build.sh command is using this tree: ${owner:-unknown}" >&2
      echo "      one build or test run at a time: wait for it (ps -p $pid), or set" >&2
      echo "      DEV_TREE_WAIT=<seconds> to queue behind it. Nothing was started." >&2
      exit 1
    fi
    waited=$((waited + 1))
    sleep 1
  done
}
tree_lock_release(){
  if [ -n "${TREE_LOCK_HELD:-}" ]; then
    rm -rf "$TREE_LOCK_DIR" 2>/dev/null
    TREE_LOCK_HELD=""
  fi
}
trap 'eng_lock_release; tree_lock_release' EXIT

# unicode_gen bootstrap (the Makefile's conditional unicode/UnicodeData.txt rule;
# inactive unless the corpus is vendored).
eng_unicode_bootstrap(){
  local need=0
  if eng_out_stale "${BIN_PFX}unicode_gen" "$OBJDIR/unicode_gen.host.o" "$OBJDIR/cutils.host.o"; then need=1; fi
  if [ "$need" = 1 ]; then
    local s1 s2 specs=""
    s1=$(eng_src_of unicode_gen) || return 1
    s2=$(eng_src_of cutils) || return 1
    specs+="$s1"$'\t'"$OBJDIR/unicode_gen.host.o"$'\t'"host"$'\t'$'\n'
    specs+="$s2"$'\t'"$OBJDIR/cutils.host.o"$'\t'"host"$'\t'$'\n'
    eng_pool "$specs" || return 1
    "$ENG_HOST_CC" $ENG_LDFLAGS $ENG_CFLAGS -o "${BIN_PFX}unicode_gen" \
      "$OBJDIR/unicode_gen.host.o" "$OBJDIR/cutils.host.o" || return 1
    echo "  LINK unicode_gen"
  fi
  if eng_out_stale "${GEN_PFX}libunicode-table.h" "${BIN_PFX}unicode_gen"; then
    "${BIN_PFX}unicode_gen" unicode "${GEN_PFX}libunicode-table.h" || return 1
    echo "  GEN libunicode-table.h"
  fi
  return 0
}

# codegen: repl.c (the REPL bytecode), hello.c / test_fib.c (examples).
# These are ARTIFACTS: --out routes them beneath OUT_BASE so a workspace build
# never writes generated C into the real tree.
eng_codegen(){
  if eng_out_stale "${GEN_PFX}repl.c" "$ENG_DYNAJSC" repl.js; then
    ENG_DID_WORK=1
    "$ENG_DYNAJSC" -s -c -o "${GEN_PFX}repl.c" -m repl.js || { echo "FAIL: repl.c codegen"; return 1; }
    echo "  GEN repl.c"
  fi
  if [ "$ENG_BUILD_EXAMPLES" = 1 ]; then
    if eng_out_stale "${GEN_PFX}hello.c" "$ENG_DYNAJSC" examples/hello.js; then
      ENG_DID_WORK=1
      "$ENG_DYNAJSC" -e $ENG_HELLO_OPTS -o "${GEN_PFX}hello.c" examples/hello.js \
        || { echo "FAIL: hello.c codegen"; return 1; }
      echo "  GEN hello.c"
    fi
    if eng_out_stale "${GEN_PFX}test_fib.c" "$ENG_DYNAJSC" examples/test_fib.js; then
      ENG_DID_WORK=1
      "$ENG_DYNAJSC" -e -M "${BIN_PFX}examples/fib.so,fib" -m -o "${GEN_PFX}test_fib.c" examples/test_fib.js \
        || { echo "FAIL: test_fib.c codegen"; return 1; }
      echo "  GEN test_fib.c"
    fi
  fi
  return 0
}

# the 0-warning policy every build.sh build always had: any compiler warning
# fails the build (two allowlisted lines: an LLVM vectorizer note and the
# openlibm archive's macOS loader warning).
eng_strict_warnings(){
  local warn
  warn=$(cat "$OBJDIR"/.build-logs/*.log "$OBJDIR"/.link-logs/*.log 2>/dev/null \
         | grep -i "warning:" \
         | grep -v "loop not vectorized" \
         | grep -v "libopenlibm\.a.*malformed LC_DYSYMTAB" || true)
  [ -z "$warn" ] && return 0
  echo "FAIL: build warnings"
  printf '%s\n' "$warn" | head -8 | sed 's/^/  /'
  return 1
}

# the CONFIG_CHECK_JSVALUE audit objects (Makefile `check-objs`). Built with the
# PLAIN config, no HARDEN/WERROR and no config-stamp wipe: in the Makefile this
# was a non-build goal (IGNORE_CFG stayed empty, no nuke) and the hardened probe
# set (-Wcast-qual) does not compile these header-heavy variants. They are audit
# artifacts, never linked.
engine_check_objs(){
  eng_parse_cfg "$@" || return 1
  eng_detect
  eng_lock_acquire || return 1
  eng_flags || { eng_lock_release; return 1; }
  mkdir -p "$OBJDIR" "$OBJDIR/examples" "$OBJDIR/tests"
  local specs="" src b
  for b in dynajs engine-parser engine-serialize dyna-cli; do
    src=$(eng_src_of "$b") || { die "check-objs: no source for $b.c"; eng_lock_release; return 1; }
    specs+="$src"$'\t'"$OBJDIR/$b.check.o"$'\t'"check"$'\t'$'\n'
  done
  eng_pool "$specs"
  local rc=$?
  eng_lock_release
  return "$rc"
}

engine_clean(){
  local exe="${ENG_EXE:-}"
  rm -f "${GEN_PFX}repl.c" "${GEN_PFX}hello.c" "${GEN_PFX}test_fib.c" \
        "${GEN_PFX}libunicode-table.h" "${BIN_PFX}unicode_gen"
  rm -f ${BIN_PFX}dynajs$exe ${BIN_PFX}dynajsc$exe ${BIN_PFX}run-test262$exe \
        ${BIN_PFX}host-dynajsc ${BIN_PFX}dyna-debug$exe ${BIN_PFX}run-test262-debug$exe
  rm -f *.a *.o *.d
  rm -f fuzz_eval fuzz_compile fuzz_regexp fuzz_regexp_compile fuzz_json fuzz_bytecode \
        fuzz_bceval fuzz_module_export fuzz_net fuzz_dyns fuzz_lz4 fuzz_scram fuzz_codec \
        fuzz_stdlib fuzz_parsers fuzz_dataframe fuzz_oauth2 fuzz_csv
  rm -f examples/hello examples/hello_module examples/test_fib examples/*.so tests/*.so
  # the effective artifact root and every variant beneath it (variants are
  # nested subdirs, so one rm -rf covers the set)
  rm -rf "$OUT_BASE" "$OUT_BASE/slim" *.dSYM
  rm -rf "$TREES" "$ROOT/.dev" 2>/dev/null
  return 0
}

# --- module scoping (test/gate) ----------------------------------------------
# A change that touched one or a few modules must not pay for the full matrix.
# The scope: TEST_SCOPE=all forces full; MODULES (env, or `MODULES=a,b` among
# the args) names it; without either, tools/affected-modules.sh derives it from
# the changed files -- core/infra/empty mean full, module names mean scoped.
# Sets MODS (empty = run full) and SCOPE_REASON (the why, for the log line).
_derive_scope(){
  MODS=""; SCOPE_REASON=""
  local a m="${MODULES:-}"
  [ "${TEST_SCOPE:-}" = all ] && { SCOPE_REASON="TEST_SCOPE=all"; return 0; }
  for a in "$@"; do
    case "$a" in
      MODULES=*) m="${a#MODULES=}" ;;
      TEST_SCOPE=all) SCOPE_REASON="TEST_SCOPE=all"; return 0 ;;
    esac
  done
  if [ -n "$(printf '%s' "$m" | tr -d ' ,')" ]; then
    MODS=$(printf '%s' "$m" | tr ',' ' ' | tr -s ' ' | sed 's/^ //')
    SCOPE_REASON="MODULES=$MODS"
    return 0
  fi
  local d
  d=$(./tools/affected-modules.sh HEAD 2>/dev/null || true)
  case " $d " in
    *" core "*)  SCOPE_REASON="core touched -- full matrix"; return 0 ;;
    *" infra "*) SCOPE_REASON="infra touched -- full matrix"; return 0 ;;
  esac
  if [ -n "$d" ]; then
    MODS="$d"; SCOPE_REASON="derived from changed files"; return 0
  fi
  SCOPE_REASON="no changes derived -- full matrix"
}

_run_diag(){
  local extra="" senv=""
  case "$1" in
    139) echo "  rc=139: segfault -- rerun as './build.sh asan ${3:-FILE}' for a stack" >&2 ;;
    124) echo "  rc=124: the run hit the ${TIMEOUT}s wall bound (a hang, not a crash)" >&2 ;;
  esac
  if grep -q "could not load module filename 'dyna:" "$2" 2>/dev/null; then
    case "$cmd" in
      asan)     extra="CONFIG_ASAN=y";  senv="ASAN_OPTIONS=detect_leaks=0 " ;;
      ubsan)    extra="CONFIG_UBSAN=y"; senv="UBSAN_OPTIONS=halt_on_error=1 " ;;
      openlibm) extra="CONFIG_OPENLIBM=y" ;;
    esac
    echo "  likely cause: this $cmd build has no CONFIG_NATIVE_MODULES, so dyna:* cannot load" >&2
    echo "  fix: ./build.sh clean && ./build.sh build ${extra:+$extra }$NATIVE_CFG && ${senv}./dynajs ${3:-FILE}" >&2
  fi
}

_warn_dyna(){
  local f="$1" extra="" senv=""
  [ -f "$f" ] || return 0
  grep -qE 'dyna:[A-Za-z]' "$f" 2>/dev/null || return 0
  case "$cmd" in
    asan)     extra="CONFIG_ASAN=y";  senv="ASAN_OPTIONS=detect_leaks=0 " ;;
    ubsan)    extra="CONFIG_UBSAN=y"; senv="UBSAN_OPTIONS=halt_on_error=1 " ;;
    openlibm) extra="CONFIG_OPENLIBM=y" ;;
    bench)    echo "warning: bench: $f imports dyna:* modules, but CONFIG_NATIVE=y builds none of them" >&2
              echo "  cause: every dyna:* import fails (or the suite skips itself) while bench still reports a number" >&2
              echo "  fix: use './build.sh quick' -- that builds CONFIG_NATIVE_MODULES=y" >&2
              return 0 ;;
  esac
  echo "warning: $cmd: $f imports dyna:* modules, but this $cmd build sets no CONFIG_NATIVE_MODULES" >&2
  echo "  cause: every dyna:* import fails (or the suite skips itself) while $cmd reports ok -- that code ran under nothing" >&2
  echo "  fix: ./build.sh clean && ./build.sh build ${extra:+$extra }$NATIVE_CFG && ${senv}./dynajs $f" >&2
}

_run_target(){
  local t="$1" log rc; shift || true
  need_file "$t"
  log=$(mktemp)
  if [ "$DEBUG" = 1 ]; then echo "[devsh] \$ ./dynajs $t $*" >&2; fi
  ./dynajs "$t" "$@" 2>&1 | tee "$log"
  rc=${PIPESTATUS[0]}
  if [ "$DEBUG" = 1 ]; then echo "[devsh] rc=$rc ./dynajs $t $*" >&2; fi
  if [ "$rc" != 0 ]; then
    echo "FAIL: $cmd: ./dynajs exited rc=$rc on $t" >&2
    _run_diag "$rc" "$log" "$t"
    rm -f "$log"; exit 1
  fi
  rm -f "$log"
}

_t262_full(){
  [ -f "$CONF" ] || { echo "FAIL: test262: $CONF missing" >&2
    echo "  bootstrap: git clone --single-branch --shallow-since=2025-09-01 https://github.com/tc39/test262.git" >&2
    echo "  (the corpus is git-ignored; ./build.sh test2-bootstrap does this for you)" >&2
    return 1; }
  local got out rc bin="${1:-$ROOT/.obj-t262/run-test262}"
  [ -x "$bin" ] || { echo "FAIL: test262: runner missing or not executable: $bin" >&2
    echo "  the gate BUILDS it before this leg; a manual ./build.sh t262 run does too." >&2
    echo "  build it: OUT=\"\$(dirname \"$bin\")\" ./build.sh build $HARDEN_CFG" >&2
    return 1; }
  [ -d test262 ] || { echo "FAIL: test262: no test262/ corpus in this tree (bootstrap it: ./build.sh test2-bootstrap)" >&2
    return 1; }
  out="$DEV_SCRATCH/t262-$$.log"; mkdir -p "$DEV_SCRATCH"
  local tmo="${T262_TIMEOUT:-900}"
  if [ "$DEBUG" = 1 ]; then echo "[devsh] \$ $bin -c $CONF -a -T $JOBS (output to $out)" >&2; fi
  # BOUNDED, and in a SUBPROCESS: a wedged or leaking runner can never take the
  # gate's own shell down with it (B1-06.2 used to run in the gate shell, so
  # its exit 1 killed the gate with no message and no g_fail_stage).
  if have timeout; then
    timeout -k 5 "$tmo" "$bin" -c "$CONF" -a -T "$JOBS" >"$out" 2>&1; rc=$?
  elif have gtimeout; then
    gtimeout -k 5 "$tmo" "$bin" -c "$CONF" -a -T "$JOBS" >"$out" 2>&1; rc=$?
  else
    ./tools/bounded-run.sh "$tmo" "$(( tmo > 600 ? 600 : tmo ))" "test262-full" -- \
      "$bin" -c "$CONF" -a -T "$JOBS" >"$out" 2>&1; rc=$?
  fi
  # The runner's OWN exit status is load-bearing and used to be discarded: a
  # crash after the last Result line still produced a parsable line.
  if [ "$rc" = 124 ] || [ "$rc" = 125 ]; then
    echo "FAIL: test262: the runner exceeded the ${tmo}s bound and was killed (exit $rc)"
    echo "  last 15 lines:"; tail -15 "$out" | sed 's/^/    /'
    echo "  full log: $out"
    return 1
  fi
  got=$(grep -oE '[0-9]+/[0-9]+ errors' "$out" | grep -oE '[0-9]+/[0-9]+')
  if [ -z "$got" ]; then
    echo "FAIL: test262: run-test262 produced no Result line (runner exit $rc)"
    echo "  likely cause: the runner crashed, or $CONF is stale; last 15 lines:"
    tail -15 "$out" | sed 's/^/    /'
    echo "  full log kept at: $out"
    return 1
  fi
  got_f=${got%/*}; got_t=${got#*/}
  base_f=${BASELINE%/*}; base_t=${BASELINE#*/}
  if [ "$got_f" -gt "$base_f" ]; then
    echo "FAIL: test262 regressed: got $got, pin allows $base_f failures (bisect with: ./build.sh t262 <subtree>)"
    tail -5 "$out" | sed 's/^/    /'
    echo "  full log: $out"
    return 1
  fi
  if [ "$got_t" -lt "$base_t" ]; then
    echo "FAIL: test262 corpus shrank: got $got, want at least */$base_t (did test262/ move? re-bootstrap test262/)"
    echo "  full log: $out"
    return 1
  fi
  # run-test262 returns `new_errors || changed_errors || fixed_errors`
  # (tools/run-test262.c), i.e. NONZERO whenever the corpus has ANY failures --
  # including the 141 this pin allows. So its exit status carries no
  # information the pin does not already have, and treating it as a verdict
  # would make the leg permanently red. What it DOES still tell us is a CRASH:
  # a signal death is rc >= 128 and the Result line can still have been
  # printed, which is exactly the case the old code could not see.
  if [ "$rc" -ge 128 ]; then
    echo "FAIL: test262: the runner was killed by a signal (exit $rc) after printing $got"
    tail -15 "$out" | sed 's/^/    /'
    echo "  full log: $out"
    return 1
  fi
  if [ "$got" = "$BASELINE" ]; then echo "test262: $got ok"
  else echo "test262: $got ok (IMPROVED past the $BASELINE pin -- lower it)"; fi
  rm -f "$out"
  return 0
}

_p_names=(); _p_logs=(); RUNDIR=
_rundir(){ [ -n "$RUNDIR" ] || RUNDIR=$(mktemp -d "${TMPDIR:-/tmp}/devsh.XXXXXX"); }
# Every gated run is bounded. `timeout`/`gtimeout` are not always installed
# (stock macOS has neither), and the old fallback ran the stage UNBOUNDED with
# no warning, which is indistinguishable from "still working" (B1-06.3). The
# fallback is tools/bounded-run.sh: a pure-shell wall+CPU bound that needs no
# external tool, and it labels a kill 124/125 instead of passing it through.
_bound(){
  if [ "$TIMEOUT" -le 0 ] 2>/dev/null; then "$@"; return $?; fi
  if have timeout;  then _v timeout  -k 5 "$TIMEOUT" "$@"; return $?; fi
  if have gtimeout; then _v gtimeout -k 5 "$TIMEOUT" "$@"; return $?; fi
  [ -x ./tools/bounded-run.sh ] || {
    echo "FAIL: no timeout tool (timeout/gtimeout) and no tools/bounded-run.sh:" >&2
    echo "      a bounded gate needs one of them; install coreutils (gtimeout) or" >&2
    echo "      restore tools/bounded-run.sh. Refusing to run UNBOUNDED." >&2
    return 1; }
  _v ./tools/bounded-run.sh "$TIMEOUT" "$(( TIMEOUT > 600 ? 600 : TIMEOUT ))" "stage:${cmd:-build.sh}" -- "$@"
}
_pstart(){
  local name="$1" log; shift
  _rundir
  log="$RUNDIR/${name//[^A-Za-z0-9._-]/_}.log"
  _p_names+=("$name"); _p_logs+=("$log")
  _log_progress "[stage: $name] starting"
  if [ "$DEBUG" = 1 ]; then echo "[devsh] \$ [stage $name] $*" >&2; fi
  if [ "$SERIAL" = 1 ]; then
    _bound "$@" >"$log" 2>&1; echo $? >"$log.rc"; return 0
  fi
  while [ "$(jobs -rp | wc -l)" -ge "$PAR" ]; do sleep 0.2; done
  { _bound "$@" >"$log" 2>&1; echo $? >"$log.rc"; } &
}
_preport(){
  local rc last errs; rc=$(cat "$2.rc" 2>/dev/null || echo 99)
  if [ "$rc" = 0 ]; then
    _log_progress "[stage: $1] ok"
    last=$(grep -vE '^[[:space:]]*$' "$2" 2>/dev/null | tail -1)
    case "$last" in "$1: "*) echo "$last" ;; *) echo "$1: ok" ;; esac
    return 0
  fi
  _log_progress "[stage: $1] FAILED (rc=$rc)"
  if   [ "$rc" = 99 ]; then echo "FAIL: $1 wrote no rc file -- killed hard (SIGKILL/OOM?) or never started; log: $2"
  elif [ "$rc" = 124 ]; then echo "FAIL: $1 TIMED OUT after ${TIMEOUT}s (raise it: DEV_TIMEOUT=3600 ./build.sh gate)"
  else echo "FAIL: $1 (rc=$rc)"; fi
  errs=$(grep -niE "error:" "$2" 2>/dev/null | head -10 || true)
  if [ -n "$errs" ]; then
    echo "  First errors (log line: text):"
    echo "$errs" | sed 's/^/    /'
  fi
  echo "  Tail of log:"
  tail -25 "$2" 2>/dev/null | sed 's/^/    /'
  echo "    full log: $2"
  return 1
}
_pwait(){
  wait
  local i bad=0
  for i in "${!_p_names[@]}"; do
    _preport "${_p_names[$i]}" "${_p_logs[$i]}" || bad=1
  done
  _p_names=(); _p_logs=()
  return "$bad"
}

CLONE=
_clone_probe(){
  local t; t=$(mktemp -d) || return 1
  : >"$t/a"
  if   cp -c "$t/a" "$t/b" 2>/dev/null;             then CLONE="cp -Rc"
  elif cp --reflink=auto "$t/a" "$t/c" 2>/dev/null; then CLONE="cp -R --reflink=auto"
  fi
  rm -rf "$t"
  [ -n "$CLONE" ]
}

TREE_NEEDS="build.sh VERSION .build-variant repl.js dynajs.d.ts install.sh README.md src tests third_party tools examples bench"

_tree(){
  local d="$TREES/$1" e
  [ "$1" = . ] && return 0
  rm -rf "$d" && mkdir -p "$d" || return 1
  for e in $TREE_NEEDS; do
    if [ ! -e "$ROOT/$e" ]; then
      echo "gate clone: TREE_NEEDS entry '$e' is missing from the repo root" >&2
      return 1
    fi
    $CLONE "$ROOT/$e" "$d/" || return 1
  done
  [ -e "$ROOT/test262" ] || { echo "gate clone: test262/ missing" >&2; return 1; }
  ln -s "$ROOT/test262" "$d/test262" || return 1
  ( cd "$d" && rm -rf .obj *.dSYM >/dev/null 2>&1 )
}

# gate stages. Build kinds run the engine IN THE CLONE (private objdir, no
# host lock); suite kinds run the ported runners against the tree they land in
# (the clone for cloned stages, the repo root for ".").
_stage(){
  local name="$1" cfg="$2" kind="$3"; shift 3
  case "$kind" in
    build)
      if [ "$name" = "." ]; then
        ( engine_build $cfg ) || return 1
      else
        cd "$TREES/$name" || return 1
        ENG_SKIP_LOCK=1 engine_build $cfg || return 1
      fi ;;
    smoke) local t; for t in "$@"; do ./dynajs "$t" </dev/null || return 1; done ;;
    pool)
      [ $# -gt 0 ] || return 0
      DEV_JOBS=${DEV_JOBS:-$NCPU} ./tools/run-tests-parallel.sh "$@" || return 1 ;;
    ctest)
      eng_objects
      _core_tests_run || return 1 ;;
    fuzz)
      fz_setup CONFIG_NATIVE_MODULES=y || return 1
      fz_audit_run || return 1
      fz_build || return 1 ;;
    tls)
      if [ "$GATE_TLS" = y ]; then
        _require_dynajs
        ./dynajs tests/test_x509.js        || return 1
        ./dynajs tests/test_crypto_aead.js || return 1
      else
        echo "tls stage: SKIPPED (no OpenSSL >= 3.0 via pkg-config)"
      fi ;;
    native)
      _native_scope_list "TEST_SCOPE=all"
      [ -n "${MODS:-}" ] && _native_scope_list "MODULES=$MODS"
      _require_native_bin
      _park_sweep_so || return 1
      echo "native stage: $(printf '%s' "$NATIVE_LIST" | wc -w | tr -d ' ') suites"
      _run_parallel $NATIVE_LIST || return 1
      sh tests/test_log_format.sh ./dynajs || return 1
      tests/run_base58_alloc.sh ./dynajs && tests/run_base58_strict.sh ./dynajs || return 1 ;;
    native-scoped)
      [ -n "${MODS:-}" ] || { echo "native-scoped stage: no MODS -- nothing to scope"; return 0; }
      _native_scope_list "MODULES=$MODS" || return 1
      _require_native_bin
      echo "native-scoped stage: $(printf '%s' "$NATIVE_LIST" | wc -w | tr -d ' ') suites for [$MODS]"
      _run_parallel $NATIVE_LIST || return 1 ;;
    tsan)
      if [ "$name" = "." ]; then
        ( engine_build CONFIG_TSAN=y $cfg ) || return 1
      else
        cd "$TREES/$name" || return 1
        ENG_SKIP_LOCK=1 engine_build CONFIG_TSAN=y $cfg || return 1
      fi ;;
    *) echo "FAIL: unknown stage kind $kind"; return 1 ;;
  esac
}

# ============================================================================
# PHASE 2 -- every remaining Makefile target, as build.sh subcommands.
#
# The build engine above compiles and links; this section is the proof
# machinery that used to be build-system targets: the native test matrix (with the
# MODULES/TEST_SCOPE/TEST_MODS scoping semantics), the core JS suite leg, the
# per-module slim/one minimal binaries (the MODDEFS_/MODOBJ_/MODTESTS_ tables),
# every standalone C regression target (verbatim-in-effect: same sources, same
# flags, same run -- the three io_uring ones keep their docker invocation),
# the fuzz link proof, the check-* documentation/install gates, install, pgo
# and the test262 bootstrap. Nothing here calls an external build system.
# ============================================================================

# plain host compiler for the standalone regression targets (they never took
# the build config's compiler selection, harden probes or ccache wrap)
reg_cc(){ have clang && { printf 'clang'; return; }; printf 'gcc'; }

MT_SLIM_MODS="random compress net structures structures3 ml simd file stream semver bytes \
crypto-hash matcher encoding time mathx csv dataframe uuid config log url \
term cli validate json schema xml yaml decimal serialize vserialize html sys \
scrape oauth2 bench stdlib-os uring"
MT_GATEONLY_MODS="http tls async"
MT_KNOWN_MODS="$MT_SLIM_MODS $MT_GATEONLY_MODS"

# The per-module tables, ported 1:1 from the Makefile (MODDEFS_/MODOBJ_/
# MODTESTS_). Every test file appears in exactly ONE mt_modtests list; the
# native matrix derives from these lists plus MULTI_TESTS, so a suite added
# to a module list is wired into the slim runner AND the full matrix at once.
mt_moddefs(){ case "$1" in
  async) printf '%s' 'ASYNC' ;;
  bench) printf '%s' 'BENCH COMPRESS CSV SIMD SYS NET URL' ;;
  bytes) printf '%s' 'BYTES ENCODING' ;;
  cli) printf '%s' 'TERM' ;;
  compress) printf '%s' 'COMPRESS CRYPTO ENCODING SYS' ;;
  config) printf '%s' 'CONFIG' ;;
  crypto-hash) printf '%s' 'CRYPTO ENCODING SYS' ;;
  csv) printf '%s' 'CSV' ;;
  dataframe) printf '%s' 'DATAFRAME' ;;
  decimal) printf '%s' 'DECIMAL' ;;
  encoding) printf '%s' 'ENCODING' ;;
  file) printf '%s' 'FILE' ;;
  html) printf '%s' 'HTML' ;;
  http) printf '%s' 'NET SYS' ;;
  json) printf '%s' 'JSON' ;;
  log) printf '%s' 'LOG' ;;
  matcher) printf '%s' 'MATCHER' ;;
  mathx) printf '%s' 'MATHX' ;;
  ml) printf '%s' 'ML CRYPTO' ;;
  net) printf '%s' 'NET SYS COMPRESS URL' ;;
  oauth2) printf '%s' 'OAUTH2 CRYPTO ENCODING' ;;
  random) printf '%s' 'RANDOM' ;;
  schema) printf '%s' 'SCHEMA' ;;
  scrape) printf '%s' 'SCRAPE SYS HTML' ;;
  semver) printf '%s' 'SEMVER' ;;
  serialize) printf '%s' 'VSERIALIZE' ;;
  simd) printf '%s' 'SIMD' ;;
  stdlib-os) printf '%s' '' ;;
  stream) printf '%s' 'STREAM COMPRESS' ;;
  structures) printf '%s' 'STRUCTURES CRYPTO' ;;
  structures3) printf '%s' 'STRUCTURES3' ;;
  sys) printf '%s' 'SYS' ;;
  term) printf '%s' 'TERM' ;;
  time) printf '%s' 'TIME' ;;
  tls) printf '%s' 'NET SYS' ;;
  uring) printf '%s' '' ;;
  url) printf '%s' 'URL' ;;
  uuid) printf '%s' 'UUID' ;;
  validate) printf '%s' 'VALIDATE' ;;
  vserialize) printf '%s' 'VSERIALIZE' ;;
  xml) printf '%s' 'XML' ;;
  yaml) printf '%s' 'YAML' ;;
  *) return 1 ;;
esac; }
mt_modobj(){ case "$1" in
  async) printf '%s' '@OBJDIR@/dyna-async.o' ;;
  bench) printf '%s' '@OBJDIR@/dyna-bench.o @OBJDIR@/dyna-compress.o @OBJDIR@/dyna-csv.o @OBJDIR@/dyna-simd.o @OBJDIR@/dyna-sys.o @OBJDIR@/dyna-url.o @OBJDIR@/dyna-crypto.o @OBJDIR@/dyna-encoding.o' ;;
  bytes) printf '%s' '@OBJDIR@/dyna-bytes.o @OBJDIR@/dyna-encoding.o' ;;
  cli) printf '%s' '@OBJDIR@/dyna-term.o' ;;
  compress) printf '%s' '@OBJDIR@/dyna-compress.o @OBJDIR@/dyna-crypto.o @OBJDIR@/dyna-encoding.o @OBJDIR@/dyna-sys.o' ;;
  config) printf '%s' '@OBJDIR@/dyna-config.o' ;;
  crypto-hash) printf '%s' '@OBJDIR@/dyna-crypto.o @OBJDIR@/dyna-encoding.o @OBJDIR@/dyna-sys.o' ;;
  csv) printf '%s' '@OBJDIR@/dyna-csv.o' ;;
  dataframe) printf '%s' '@OBJDIR@/dyna-dataframe.o' ;;
  decimal) printf '%s' '@OBJDIR@/dyna-decimal.o' ;;
  encoding) printf '%s' '@OBJDIR@/dyna-encoding.o' ;;
  file) printf '%s' '@OBJDIR@/dyna-file.o' ;;
  html) printf '%s' '@OBJDIR@/dyna-html.o' ;;
  http) printf '%s' '@OBJDIR@/dyna-sys.o @OBJDIR@/dyna-compress.o @OBJDIR@/dyna-url.o' ;;
  json) printf '%s' '@OBJDIR@/dyna-json.o' ;;
  log) printf '%s' '@OBJDIR@/dyna-log.o' ;;
  matcher) printf '%s' '@OBJDIR@/dyna-matcher.o' ;;
  mathx) printf '%s' '@OBJDIR@/dyna-mathx.o' ;;
  ml) printf '%s' '@OBJDIR@/dyna-ml.o @OBJDIR@/dyna-crypto.o' ;;
  net) printf '%s' '@OBJDIR@/dyna-sys.o @OBJDIR@/dyna-compress.o @OBJDIR@/dyna-url.o' ;;
  oauth2) printf '%s' '@OBJDIR@/dyna-oauth2.o @OBJDIR@/dyna-crypto.o @OBJDIR@/dyna-encoding.o' ;;
  random) printf '%s' '@OBJDIR@/dyna-random.o' ;;
  schema) printf '%s' '@OBJDIR@/dyna-schema.o @OBJDIR@/dyna-vserialize.o @OBJDIR@/dyna-protobuf.o @OBJDIR@/dyna-asn1.o' ;;
  scrape) printf '%s' '@OBJDIR@/dyna-scrape.o @OBJDIR@/dyna-sys.o @OBJDIR@/dyna-html.o' ;;
  semver) printf '%s' '@OBJDIR@/dyna-semver.o' ;;
  serialize) printf '%s' '@OBJDIR@/dyna-vserialize.o @OBJDIR@/dyna-protobuf.o @OBJDIR@/dyna-asn1.o' ;;
  simd) printf '%s' '@OBJDIR@/dyna-simd.o' ;;
  stdlib-os) printf '%s' '' ;;
  stream) printf '%s' '@OBJDIR@/dyna-stream.o @OBJDIR@/dyna-compress.o' ;;
  structures) printf '%s' '@OBJDIR@/dyna-structures.o @OBJDIR@/dyna-serialize.o @OBJDIR@/dyna-graph.o @OBJDIR@/dyna-crypto.o' ;;
  structures3) printf '%s' '@OBJDIR@/dyna-structures3.o' ;;
  sys) printf '%s' '@OBJDIR@/dyna-sys.o' ;;
  term) printf '%s' '@OBJDIR@/dyna-term.o' ;;
  time) printf '%s' '@OBJDIR@/dyna-time.o' ;;
  tls) printf '%s' '@OBJDIR@/dyna-sys.o @OBJDIR@/dyna-compress.o @OBJDIR@/dyna-url.o' ;;
  uring) printf '%s' '' ;;
  url) printf '%s' '@OBJDIR@/dyna-url.o' ;;
  uuid) printf '%s' '@OBJDIR@/dyna-uuid.o' ;;
  validate) printf '%s' '@OBJDIR@/dyna-validate.o' ;;
  vserialize) printf '%s' '@OBJDIR@/dyna-vserialize.o @OBJDIR@/dyna-protobuf.o @OBJDIR@/dyna-asn1.o' ;;
  xml) printf '%s' '@OBJDIR@/dyna-xml.o' ;;
  yaml) printf '%s' '@OBJDIR@/dyna-yaml.o' ;;
  *) return 1 ;;
esac; }
mt_modtests(){ case "$1" in
  async) printf '%s' 'tests/test_async.js' ;;
  bench) printf '%s' 'tests/test_bench.js tests/test_perf_acceptance.js tests/test_perf_adversarial.js' ;;
  bytes) printf '%s' 'tests/test_bytes.js tests/test_bytes_handle.js tests/test_bytes_accessors.js  tests/test_iconv.js tests/test_text_kernels.js tests/test_utf16.js tests/test_text_simd.js tests/test_bytes_search_bulk.js tests/test_dt8_utf8_forms.js' ;;
  cli) printf '%s' 'tests/test_cli.js' ;;
  compress) printf '%s' 'tests/test_compress.js tests/test_dictionary.js tests/test_lz4.js  tests/test_codec_zstd.js tests/test_lz4_unframe_refusals.js tests/test_lz4_refusal_matrix.js tests/test_lz4_interop.js tests/test_a4_compress_caps.js' ;;
  config) printf '%s' 'tests/test_config.js tests/test_toml.js tests/test_config_upgrade.js tests/test_toml_leak.js tests/test_a4_env_bounds.js' ;;
  crypto-hash) printf '%s' 'tests/test_hash_keyed.js  tests/test_crypto.js tests/test_crypto_reuse.js tests/test_jwt_confusion.js tests/test_crypto_standalone.js tests/test_crypto_aead.js tests/test_crypto_curve.js tests/test_crypto_otp.js tests/test_hash_split.js tests/test_sha3.js tests/test_blake.js tests/test_x509.js tests/test_crypto_surface.js tests/test_hasher_lifecycle.js tests/test_crypto_upgrade.js tests/test_hash_upgrade.js tests/test_crypto_strict_opts.js tests/test_crypto_bounds.js tests/test_crypto_pem_rfc.js tests/test_hash_keyed_leak.js tests/test_xxh32_spec.js' ;;
  csv) printf '%s' 'tests/test_csv_formula_injection.js tests/test_csv.js tests/test_csv_parse.js' ;;
  dataframe) printf '%s' 'tests/test_dataframe.js tests/test_dataframe_audit.js  tests/test_dataframe_inplace.js tests/review_df_colcol.js tests/test_dataframe_churn.js tests/test_dataframe_rolling_masked.js tests/test_dataframe_join_pivot_caps.js tests/test_dataframe_result_ownership.js tests/test_dataframe_group_keys.js tests/test_df_info_oom.js tests/test_a4_group_bit_sign.js' ;;
  decimal) printf '%s' 'tests/test_decimal.js tests/test_decimal_audit.js  tests/test_decimal_upgrade.js tests/test_decimal_strict.js tests/test_money_from.js' ;;
  encoding) printf '%s' 'tests/test_encoding.js tests/test_basex.js tests/test_json5.js  tests/test_jsonpath.js tests/test_qr.js tests/test_encoding_detect_fallback.js tests/test_encoding_into_at.js tests/test_encoding_encode_into.js' ;;
  file) printf '%s' 'tests/test_file.js tests/test_file_handle.js  tests/test_file_lock.js tests/test_file_copy.js tests/test_watch.js tests/test_file_bytes.js tests/test_glob_match.js tests/test_watch_iter.js tests/test_watch_oom.js' ;;
  html) printf '%s' 'tests/test_sanitizer_urls.js tests/test_html.js tests/test_html_text.js tests/test_html_pentest.js  tests/test_markdown.js tests/test_template.js tests/test_template_bounds.js tests/test_html_hardening.js tests/test_html_rewritelinks.js tests/test_a4_sanitizer_policy.js' ;;
  http) printf '%s' 'tests/test_web_compat.js tests/test_client_tls.js tests/test_onconnect_hook.js tests/test_http_stream.js tests/test_http_wire_framing.js tests/test_http_client_guards.js tests/test_http_onconnect_async.js tests/test_http_hdr_injection.js tests/test_http_client_framing.js  tests/test_http_fetch_creds.js tests/test_http_proxy_inject.js tests/test_http_proxy_framing.js  tests/test_http_proxy_method.js tests/test_http_status_range.js' ;;
  json) printf '%s' 'tests/test_json.js tests/test_json_audit.js  tests/test_json_ndjson.js tests/test_json_ndjson_leak.js' ;;
  log) printf '%s' 'tests/test_logger_security.js tests/test_log.js tests/test_log_rollover.js tests/test_log_properties.js  tests/test_log_extreme.js tests/test_log_upgrade.js' ;;
  matcher) printf '%s' 'tests/test_matcher.js tests/test_approx_match.js tests/test_diff.js' ;;
  mathx) printf '%s' 'tests/test_mathx.js tests/test_mathx_tierb.js tests/test_mathx_matlab.js  tests/test_mathx_stats.js tests/test_expr.js' ;;
  ml) printf '%s' 'tests/test_ml.js tests/test_ml_metrics.js tests/test_ml_weights.js tests/test_ml_into.js tests/test_ml_audit.js tests/test_ml_pipeline.js tests/test_ml_selection.js tests/test_ml_sklearn.js tests/test_ml_boosting.js tests/test_ml_decomposition.js tests/test_ml_neighbors.js tests/test_ml_preprocessing.js tests/test_ml_trees.js tests/test_ml_svm.js tests/test_ml_persist.js tests/test_ml_tree_proba.js tests/test_ml_production.js tests/test_ml_xgb.js tests/test_ml_sparse.js tests/test_ml_fit_parity.js tests/oracle_ml_hist.js tests/review_csr_malform.js tests/review_csr_lifetime.js tests/review_csr_dupsum.js tests/review_csr_estimators.js tests/review_esc_fit.js tests/review_esc_fit2.js tests/review_esc_finite.js tests/review_esc_kernel.js tests/review_esc_widths.js tests/review_leakthrow.js tests/test_ml_linreg_exact.js tests/test_ml_weights_churn.js tests/test_ml_depth_cap.js tests/test_ml_fitted_gate.js tests/test_ml_deser_validate.js tests/test_ml_finite_scan.js tests/test_ml_svc_batched.js tests/test_ml_csr_reassoc.js tests/test_ml_dbscan_grid.js tests/test_ml_dbscan_forge.js tests/test_ml_crafted_records.js tests/test_ml_persist_scale.js' ;;
  net) printf '%s' 'tests/test_net_tcp.js tests/test_net_tcp_bounds.js tests/test_net_pg_auth_hostile.js tests/test_net_sqlite_bounds.js tests/test_net_dns.js tests/test_net_sqlite.js tests/test_net_pentest.js tests/test_proxy.js tests/test_net_redis.js tests/test_net_pg.js tests/test_net_pg_stmt.js tests/test_net_pg_binary.js tests/test_net_e2e.js tests/test_net_fragment.js tests/test_net_rss.js tests/test_net_liveness.js tests/test_http_binary_body.js tests/test_pct_core.js tests/test_regexp_wide_anchor.js tests/test_net_eyeballs.js tests/test_net_fdchurn.js tests/test_net_tcp_close_recv.js tests/test_net_udp_selfclose.js tests/test_net_teardown_churn.js tests/test_net_udp_grave.js tests/test_netip.js tests/test_ratelimit.js tests/test_metrics.js tests/test_net_upgrade.js tests/test_connect_resolve.js tests/test_db_lifecycle.js tests/test_evloop.js tests/test_fetch.js tests/test_fetch_body.js tests/test_keepalive.js tests/test_multipart.js tests/test_httpmsg.js tests/test_rpc_params.js tests/test_route_static.js tests/test_http.js tests/test_http_async.js tests/test_http_async_bounds.js tests/test_http_client_async.js tests/test_http_sse.js tests/test_http_compress.js tests/test_http_metrics_endpoint.js tests/test_http_hardening.js tests/test_http_ws.js tests/test_ws_client.js tests/test_http_proxy.js tests/test_http_params.js tests/test_http_pentest.js tests/test_http_security.js tests/test_http_upload.js tests/test_http_upload_async.js tests/test_static_traversal.js tests/test_netfile_pentest.js tests/test_tls_server.js tests/test_module_interop.js tests/review_dns_ttl.js tests/test_http_perf_regress.js tests/test_http_pipeline_cap.js tests/test_net_resp_int.js tests/test_net_resp_depth.js tests/test_net_pg_rowtemplate.js tests/test_http_app_workers.js tests/test_http_routes.js tests/test_http_middleware.js tests/test_http_pending_promises.js tests/test_http_stream_pump.js tests/test_http_stream_pipeline.js tests/test_http_read_reject.js tests/test_http_read_thenable.js tests/test_metrics_custom_buckets.js tests/test_metrics_lock_latency.js tests/test_httpmsg_churn_leak.js' ;;
  oauth2) printf '%s' 'tests/test_oauth2.js tests/test_oauth2_strict.js' ;;
  random) printf '%s' 'tests/test_random.js tests/test_random_audit.js tests/test_random_dist.js' ;;
  schema) printf '%s' 'tests/test_schema.js tests/test_schema_ref_paths.js tests/test_schema_audit.js tests/test_schema_upgrade.js tests/test_schema_cache_poison.js' ;;
  scrape) printf '%s' 'tests/test_dns_rebinding.js tests/test_scrape_robots.js tests/test_scrape_extract.js tests/test_scrape_modern.js tests/test_scrape_state.js tests/test_scrape_stream.js tests/test_scrape_concurrency.js tests/review_crawl_conc.js tests/test_scrape_fetcher_simple.js tests/test_scrape_sitemap.js tests/test_scrape_sitemap_bounds.js tests/test_scrape_framing.js tests/test_scrape_concurrency_strict.js tests/test_scrape_close_lifetime.js tests/test_fetcher_crlf.js tests/test_scrape_teardown_churn.js tests/test_shutdown_parked.js tests/test_shutdown_repark.js' ;;
  semver) printf '%s' 'tests/test_semver.js tests/test_semver_upgrade.js' ;;
  serialize) printf '%s' 'tests/test_vserialize.js tests/test_protobuf.js tests/test_asn1.js tests/test_asn1_bounds.js  tests/test_asn1_sz.js tests/test_asn1_hostile.js tests/test_clone_views.js' ;;
  simd) printf '%s' 'tests/test_simd.js tests/test_simd_f64.js tests/test_simd_int.js tests/test_simd_stats.js tests/test_w3_simd.js' ;;
  stdlib-os) printf '%s' '' ;;
  stream) printf '%s' 'tests/test_stream.js tests/test_stream_pipe_teardown.js tests/test_stream_hostile_thenable.js' ;;
  structures) printf '%s' 'tests/test_plan_coverage.js tests/test_structures.js tests/test_structures_iter.js  tests/test_structures_guava.js tests/test_structures_serialize.js tests/test_structures_serde.js tests/test_structures_bitset_codec.js tests/test_structures_sorted_codec.js tests/test_structures_hll.js tests/test_structures_trie_codec.js tests/test_structures_numeric_codec.js tests/test_structures_heap_natural.js tests/test_structures_multiset_codec.js tests/test_structures_table_slice.js tests/test_structures_btree.js tests/test_structures_trie_paths.js tests/test_structures_record_codecs.js tests/test_structures_itree_pending.js tests/test_structures_adversarial.js tests/test_structures_codec_forge.js tests/test_structures_graph.js tests/test_structures_graph_into.js tests/test_structures_gaps.js tests/test_lru_ttl.js tests/test_structures_hostile_forged.js tests/test_structures_bomb_budget.js' ;;
  structures3) printf '%s' '' ;;
  sys) printf '%s' 'tests/test_sys.js tests/test_proc.js tests/test_spawn.js tests/test_sys_machine.js  tests/test_spawn_lifetime.js tests/test_sys_memory.js tests/test_module_surface.js tests/test_module_gating.js tests/test_exec_pathcwd.js tests/test_sys_upgrade.js tests/test_spawn_read_detach.js' ;;
  term) printf '%s' 'tests/test_term.js' ;;
  time) printf '%s' 'tests/test_time.js tests/test_time_format.js tests/test_time_dateparser.js  tests/test_rrule.js tests/test_temporal.js tests/test_time_twins.js tests/test_time_offset.js tests/test_time_strict.js tests/test_time_nsec_strict.js tests/test_duration_bounds.js' ;;
  tls) printf '%s' '' ;;
  uring) printf '%s' 'tests/test_uring_disk.js tests/test_uring_bytes.js' ;;
  url) printf '%s' 'tests/test_idna_contexto.js tests/test_url.js tests/test_idna.js tests/test_url_host.js  tests/test_pct_core.js tests/test_url_setters.js tests/test_url_strict.js tests/test_url_host_boundary.js tests/test_url_parse_fuzz.js' ;;
  uuid) printf '%s' 'tests/test_uuid.js tests/test_ids.js tests/test_uuid_ids.js' ;;
  validate) printf '%s' 'tests/test_validate.js' ;;
  vserialize) printf '%s' '' ;;
  xml) printf '%s' 'tests/test_xml.js tests/test_xml_text.js  tests/test_xml_large.js tests/test_xml_upgrade.js tests/test_xml_pull.js tests/test_xml_writer.js tests/test_a4_xml_sax_bounds.js' ;;
  yaml) printf '%s' 'tests/test_yaml.js tests/test_yaml_scalar.js tests/test_yaml_upgrade.js  tests/test_yaml_emitter_opts.js tests/test_yaml_stream.js tests/test_yaml_folding.js' ;;
  *) return 1 ;;
esac; }

# mt_modobj_out: the MODOBJ_ list with the objdir prefix applied (the table
# stores @OBJDIR@ markers so the prefix follows the active configuration).
mt_modobj_out(){
  local m="$1" v
  v=$(mt_modobj "$m") || return 1
  printf '%s' "${v//@OBJDIR@/${OBJDIR:-.obj}}"
}

MT_MULTI_TABLE="tests/test_data_w3_fixes.js tests/test_data_low_fixes.js tests/test_data_medium_fixes.js tests/test_conformance.js tests/test_algo_blackbox.js tests/test_capabilities.js tests/test_archive.js tests/test_archive_pax.js tests/test_codec_zstd.js tests/test_iterator_gap.js tests/test_globals.js tests/oracle_compress_bytes.js tests/test_pool.js tests/test_regexp_backtrack.js tests/test_native_memory.js tests/test_a4_lookahead_budget.js tests/test_a4_setproto_proxy.js tests/test_audit_p1.js tests/test_p2_batch1.js tests/test_p2_batch2.js tests/test_p2_batch3.js tests/probe_audit_leads2.js tests/test_cov_bytes_compress_encoding_hash.js tests/test_cov_file_html_xml_yaml_json.js tests/test_cov_net_http_url_validate.js tests/test_cov_ml_dataframe_mathx.js tests/test_cov_sys_time_random_uuid_misc.js tests/test_data_pentest.js tests/test_crypto_jwt.js tests/test_jwt_asym.js tests/test_validate_ext.js tests/test_file_platform.js tests/test_tls_matrix.js tests/test_tls_roots.js tests/test_scrape_crawl.js tests/test_scrape_fetcher.js tests/test_random_inputs.js tests/test_dos_guards.js tests/test_platform_hardening.js tests/test_worker_native.js tests/test_optguide_regressions.js tests/test_enum_order.js tests/test_native_subclass.js tests/test_no_prototypes.js tests/blackbox/bb_async.js tests/blackbox/bb_bench.js tests/blackbox/bb_bytes.js tests/blackbox/bb_cli.js tests/blackbox/bb_compress.js tests/blackbox/bb_config.js tests/blackbox/bb_crypto.js tests/blackbox/bb_csv.js tests/blackbox/bb_dataframe.js tests/blackbox/bb_decimal.js tests/blackbox/bb_encoding.js tests/blackbox/bb_file.js tests/blackbox/bb_globals.js tests/blackbox/bb_hash.js tests/blackbox/bb_html.js tests/blackbox/bb_http.js tests/blackbox/bb_json.js tests/blackbox/bb_log.js tests/blackbox/bb_matcher.js tests/blackbox/bb_mathx.js tests/blackbox/bb_ml.js tests/blackbox/bb_net.js tests/blackbox/bb_oauth2.js tests/blackbox/bb_proto_number_object_date.js tests/blackbox/bb_proto_string_array.js tests/blackbox/bb_random.js tests/blackbox/bb_schema.js tests/blackbox/bb_scrape.js tests/blackbox/bb_semver.js tests/blackbox/bb_serialize.js tests/blackbox/bb_simd.js tests/blackbox/bb_std_os.js tests/blackbox/bb_stream.js tests/blackbox/bb_structures.js tests/blackbox/bb_sys.js tests/blackbox/bb_time.js tests/blackbox/bb_uring.js tests/blackbox/bb_url.js tests/blackbox/bb_uuid.js tests/blackbox/bb_validate.js tests/blackbox/bb_xml.js tests/blackbox/bb_yaml.js tests/test_contract_docreqs.js tests/test_fs_rmrf_deep.js tests/test_audit_budget_1.js tests/test_audit_budget_2.js tests/test_audit_budget_3.js tests/test_audit_http_1.js tests/test_audit_http_2.js tests/test_audit_http_3.js tests/test_audit_http_4.js tests/test_audit_http_5.js tests/test_audit_http_6.js tests/test_audit_w2_engine_1.js tests/test_audit_w2_engine_2.js tests/test_audit_w2_native_1.js tests/test_audit_w2_native_2.js tests/test_audit_w3_native_1.js tests/test_audit_w3_native_2.js tests/test_audit_w3_native_3.js tests/test_audit_w3_simd.js tests/test_audit_w3_mathx.js tests/test_audit_w3_native_4.js tests/test_audit_w3_native_5.js tests/test_audit_w3_native_6.js tests/test_audit_w3_native_7.js tests/test_audit_w3_native_8.js tests/test_audit_w3_native_9.js tests/test_audit_w3_native_10.js tests/test_audit_w4_native_1.js tests/test_kit.js tests/test_audit_dbnet_1.js tests/test_audit_dbnet_2.js tests/test_audit_engine_1.js tests/test_audit_engine_2.js tests/test_audit_engine_3.js tests/test_audit_livesrv_1.js tests/test_audit_nat_1.js"

MT_STANDALONE_TABLE="tests/test_algo_blackbox.js tests/test_bytes_accessors.js tests/test_connect_resolve.js tests/test_crypto_aead.js tests/test_crypto_curve.js tests/test_data_pentest.js tests/test_html_pentest.js tests/test_http.js tests/test_http_async.js tests/test_http_params.js tests/test_http_ws.js tests/test_jwt_asym.js tests/test_net_fdchurn.js tests/test_netfile_pentest.js tests/test_scrape_crawl.js tests/test_scrape_fetcher.js tests/test_static_traversal.js tests/test_tls_matrix.js tests/test_tls_server.js tests/test_uring_disk.js tests/test_x509.js tests/test_scrape_autoclient.js"

# The nine test_audit_{budget,http}_*.js regression suites live in MULTI, not in
# a per-module list, and that placement is load-bearing:
#  - they each need MORE THAN ONE module (budget_3 needs dyna:sys +
#    dyna:structures; http_6 needs dyna:net + dyna:file). A per-module entry
#    puts the suite into that module's SLIM-binary matrix (mt_modtests feeds
#    both), and `net`'s slim object list has no dyna-file.o -- the suite would
#    fail there for a reason that has nothing to do with the regression.
#  - MULTI_TESTS is the cross-module list the FULL native matrix runs against a
#    build that has every module, which is what these suites need.
#  - the six http_* suites bind 127.0.0.1:0 and start a real server, so the
#    runner's SOLO_RE (their text mentions TCPServer/HTTPServer) already puts
#    them in the serial tail rather than the parallel wave.
#  - budget_1 declares '// timeout: 600': it runs 27 child processes each armed
#    with its own --timeout-ms deadline, and measured 242s on the base, so the
#    runner's 120s DEFAULT bound would have killed it.
CORE_TESTS_TABLE="tests/test_closure.js tests/test_language.js tests/test_modern.js tests/test_disposable.js tests/test_using_double_fire.js tests/test_engine_audit_regress.js tests/test_engine_adversarial.js tests/test_w3_parser.js tests/test_w3_atoms_arrays.js tests/test_array_ext.js tests/test_iterator_lazy.js tests/test_array_sorted.js tests/test_typedarray_ext.js tests/test_string_fill.js tests/test_date_json_fmt.js tests/test_array_dense_get.js tests/test_array_search.js tests/test_string_ext.js tests/test_string_ext2.js tests/test_string_ansi.js tests/test_ext_batch7.js tests/test_ext_batch8.js tests/oracle_parse_string.js tests/oracle_parse_ident.js tests/oracle_line_col.js tests/oracle_expr_precedence.js tests/oracle_asi.js tests/test_date_ext.js tests/test_lens_ext.js tests/test_number_ext.js tests/test_number_ext2.js tests/test_object_ext.js tests/test_object_ext2.js tests/test_function_ext.js tests/test_fn_timers.js tests/test_optimizer.js tests/test_loop.js tests/test_bigint.js tests/test_bigint_asuintn.js tests/probe_audit_leads.js tests/test_textcodec.js tests/test_textdecoder_stream.js tests/test_string_hash.js tests/test_cyclic_import.js tests/test_worker.js"

SECURITY_TESTS_TABLE="tests/test_http_params.js tests/test_parser_pentest.js tests/test_module_pentest.js tests/test_ext_pentest.js tests/test_netfile_pentest.js tests/test_html_pentest.js tests/test_static_traversal.js tests/test_net_pentest.js tests/test_http_pentest.js tests/test_data_pentest.js tests/sec_dataframe_detach.js"

API_TESTS_TABLE="tests/test_api_surface.js tests/test_api_params.js tests/test_api_differential.js tests/test_api_roundtrip.js tests/test_api_vectors.js tests/test_api_kernels.js tests/test_api_properties.js tests/test_api_fuzz.js"

# NATIVE_TESTS: the wide gate's list -- every module table's suites (the
# SLIM_MODS names plus the GATEONLY aliases) minus STANDALONE_TESTS, plus
# MULTI_TESTS. Order and duplicates preserved exactly as the derived Makefile
# list had them; MULTI_TESTS is overridable through the environment (the gate
# uses that for a one-suite debugging run).
MT_MULTI="${MULTI_TESTS:-$MT_MULTI_TABLE}"
native_tests(){
  local m t out=""
  for m in $MT_SLIM_MODS $MT_GATEONLY_MODS; do
    mt_modtests "$m" >/dev/null 2>&1 && out="$out $(mt_modtests "$m")"
  done
  NATIVE_TESTS=""
  for t in $out; do
    case " $MT_STANDALONE_TABLE " in *" $t "*) continue ;; esac
    NATIVE_TESTS="$NATIVE_TESTS $t"
  done
  NATIVE_TESTS="${NATIVE_TESTS# } $MT_MULTI"
}

# --- scoped-run plumbing -------------------------------------------------------
# test-native's three knobs, in the Makefile's precedence: TEST_SCOPE=all runs
# everything; TEST_MODS=a,b unions those modules' mt_modtests lists (unknown
# names fail); MODULES=a,b keeps every suite whose path contains test_<m> or
# bb_<m>; with NONE of them the target FAILS, printing the module list derived
# from tools/affected-modules.sh and the exact escapes.
# _split_run_args [mods] -- -> P2_CFG (CONFIG_ args), P2_SCOPE (scope args).
# With "mods", POSITIONAL arguments are module selectors: `test-native csv net`
# is exactly `test-native MODULES=csv,net` (same substring filter semantics).
_split_run_args(){
  local allow_mods=0
  [ "${1:-}" = mods ] && { allow_mods=1; shift; }
  P2_CFG=""; P2_SCOPE=""; local mods="" a
  for a in "$@"; do
    case "$a" in
      CONFIG_*) P2_CFG="$P2_CFG $a" ;;
      MODULES=*|TEST_SCOPE=*|TEST_MODS=*) P2_SCOPE="$P2_SCOPE $a" ;;
      *) if [ "$allow_mods" = 1 ]; then
           mods="$mods $a"
         else
           die "${cmd:-run}: not a CONFIG_*/MODULES=/TEST_SCOPE=/TEST_MODS= argument: '$a'"
         fi ;;
    esac
  done
  if [ -n "$mods" ]; then
    mods=$(printf '%s' "$mods" | tr ' ' ',' | tr -s ',' ',' | sed 's/^,//; s/,$//')
    P2_SCOPE="$P2_SCOPE MODULES=$mods"
  fi
  # the documented env knobs fill in when the arguments carry no explicit
  # scope: MODULES=csv,crypto ./build.sh test-native (args win over env)
  if [ -z "$P2_SCOPE" ]; then
    if [ "${TEST_SCOPE:-}" = all ]; then P2_SCOPE="TEST_SCOPE=all"
    elif [ -n "${MODULES:-}" ]; then P2_SCOPE="MODULES=$(printf '%s' "$MODULES" | tr -s ' ,' ' ,')"
    fi
  fi
  P2_CFG="${P2_CFG# }"; P2_SCOPE="${P2_SCOPE# }"
}
# true when the scope (env or args) is the deliberate full run
_scope_all_p(){
  [ "${TEST_SCOPE:-}" = all ] && return 0
  local a; for a in ${1:-}; do [ "$a" = "TEST_SCOPE=all" ] && return 0; done
  return 1
}
# the fail-closed refusal: derived module list + every escape, then exit 1
_scope_refusal(){
  local d
  d=$(./tools/affected-modules.sh HEAD 2>/dev/null || true)
  {
    echo "$1 refuses an unscoped run: the suite matrix never runs implicitly."
    echo ""
    echo "  modules affected by THIS tree (tools/affected-modules.sh HEAD):"
    echo "    ${d:-(none detected -- no changed files)}"
    echo ""
    echo "  scoped run:       ./build.sh $1 MODULES=<m1,m2,...>"
    echo "                    (positional names are the same thing: ./build.sh $1 csv crypto)"
    echo "  deliberate full:  ./build.sh $1 TEST_SCOPE=all"
  } >&2
  exit 1
}
_native_scope_list(){  # $1 = scope args string -> NATIVE_LIST (dies when unscoped)
  local a m t modsscope="" testmods="" scopeall="" cand out
  for a in $1; do
    case "$a" in
      TEST_SCOPE=*) scopeall=1 ;;
      MODULES=*)    modsscope="${a#MODULES=}" ;;
      TEST_MODS=*)  testmods="${a#TEST_MODS=}" ;;
    esac
  done
  native_tests
  if [ -n "$scopeall" ]; then
    NATIVE_LIST="$NATIVE_TESTS"
    return 0
  fi
  if [ -n "$testmods" ]; then
    NATIVE_LIST=""
    for m in $(printf '%s' "$testmods" | tr ',' ' '); do
      cand=$(mt_modtests "$m" 2>/dev/null) || die "unknown TEST_MODS module(s): $m (names are the dyna:<m> family names; e.g. net, crypto-hash, http)"
      NATIVE_LIST="$NATIVE_LIST $cand"
    done
    NATIVE_LIST="${NATIVE_LIST# }"
    [ -n "$NATIVE_LIST" ] || die "TEST_MODS=\"$testmods\" selects no suites: every named module has an empty list"
    return 0
  fi
  if [ -n "$modsscope" ]; then
    out=""
    for t in $NATIVE_TESTS; do
      for m in $(printf '%s' "$modsscope" | tr ',' ' '); do
        case "$t" in
          *test_${m}*|*bb_${m}*) out="$out $t"; break ;;
        esac
      done
    done
    NATIVE_LIST=$(printf '%s' "$out" | tr ' ' '\n' | sed '/^$/d' | sort -u | tr '\n' ' ')
    NATIVE_LIST="${NATIVE_LIST% }"
    [ -n "$NATIVE_LIST" ] || die "MODULES=\"$modsscope\" selects no suites: no native-suite path contains test_<m> or bb_<m> for any named module. Check the spelling (module names are the dyna:<m> family names -- e.g. net, not net_tcp); a source file with no dedicated suite has no scoped coverage, which is what TEST_SCOPE=all is for"
    return 0
  fi
  _scope_refusal "test-native"
}

# The core JS leg's scope: TEST_SCOPE=all keeps the whole CORE_TESTS table;
# MODULES=a,b / positional names keep only suites matching test_<m>/bb_<m>.
# An empty filtered table is a LOUD skip, not a silent full run.
_core_scope_list(){  # $1 = scope args -> CORE_LIST (+ CORE_FULL=1 for all)
  local a modsscope="" t m out=""
  CORE_FULL=0
  for a in $1; do
    case "$a" in TEST_SCOPE=*) CORE_FULL=1 ;; MODULES=*) modsscope="${modsscope} ${a#MODULES=}" ;; esac
  done
  [ "${TEST_SCOPE:-}" = all ] && CORE_FULL=1
  if [ "$CORE_FULL" = 1 ] || [ -z "${modsscope// /}" ]; then
    [ "$CORE_FULL" = 1 ] && { CORE_LIST="$CORE_TESTS_TABLE"; return 0; }
    CORE_LIST="$CORE_TESTS_TABLE"
    return 0
  fi
  for t in $CORE_TESTS_TABLE; do
    for m in $(printf '%s' "$modsscope" | tr ',' ' '); do
      case "$t" in *test_${m}*|*bb_${m}*) out="$out $t"; break ;; esac
    done
  done
  CORE_LIST=$(printf '%s' "$out" | tr ' ' '\n' | sed '/^$/d' | sort -u | tr '\n' ' ')
  CORE_LIST="${CORE_LIST% }"
  return 0
}

# --- shared legs ---------------------------------------------------------------
_park_sweep_so(){
  # tests/park_sweep.so -- a native-module fixture two suites load (test-native
  # and test-fixtures). Pic-compiled like the engine's own .so modules.
  if [ -f tests/park_sweep.so ] && ! eng_out_stale tests/park_sweep.so "$OBJDIR/tests/park_sweep.pic.o"; then
    return 0
  fi
  [ -f tests/park_sweep.c ] || { echo "FAIL: tests/park_sweep.c missing"; return 1; }
  eng_pool "tests/park_sweep.c"$'\t'"$OBJDIR/tests/park_sweep.pic.o"$'\t'"pic"$'\t'$'\n' || return 1
  local ldflags=""
  [ "$(uname -s 2>/dev/null || echo unknown)" = Darwin ] && ldflags="-undefined dynamic_lookup"
  $ENG_CC $ENG_LDFLAGS -shared $ldflags -o tests/park_sweep.so "$OBJDIR/tests/park_sweep.pic.o" $ENG_LIBS \
    || { echo "FAIL: tests/park_sweep.so link"; return 1; }
  echo "  LINK tests/park_sweep.so"
}
_bjson_so(){
  if [ -f tests/bjson.so ] && ! eng_out_stale tests/bjson.so "$OBJDIR/tests/bjson.pic.o"; then
    return 0
  fi
  eng_pool "tests/bjson.c"$'\t'"$OBJDIR/tests/bjson.pic.o"$'\t'"pic"$'\t'$'\n' || return 1
  $ENG_CC $ENG_LDFLAGS -shared -o tests/bjson.so "$OBJDIR/tests/bjson.pic.o" $ENG_LIBS \
    || { echo "FAIL: tests/bjson.so link"; return 1; }
  echo "  LINK tests/bjson.so"
}
# --- t: named suites, named cases, no build ----------------------------------
# The narrowest run there is: the suite files you name (and, inside a kit suite,
# only the cases you name) against the binary that is ALREADY built. It never
# compiles, so it refuses a binary older than the sources instead of testing
# the wrong code. A name resolves as a path, then tests/<name>, then
# tests/test_<name>.js; `mod:<m>` expands to that module's suite list.
#   ./build.sh t test_csv --case=quoted         one case of one suite
#   ./build.sh t tests/test_audit_w3_native_9.js --list
#   ./build.sh t mod:csv --bail
_t_resolve(){  # name -> T_FILES += resolved paths (dies with near matches)
  local n="$1" c m near
  case "$n" in
    mod:*) m="${n#mod:}"
           c=$(mt_modtests "$m" 2>/dev/null) || die "t: unknown module '$m' (known: $MT_KNOWN_MODS)"
           [ -n "$c" ] || die "t: module '$m' has an empty suite list"
           T_FILES="$T_FILES $c"; return 0 ;;
  esac
  for c in "$n" "tests/$n" "tests/$n.js" "tests/test_$n.js" "tests/blackbox/$n.js" "tests/blackbox/bb_$n.js"; do
    [ -f "$c" ] && { T_FILES="$T_FILES $c"; return 0; }
  done
  near=$(ls tests/*"$n"*.js tests/blackbox/*"$n"*.js 2>/dev/null | head -8 | tr '\n' ' ')
  die "t: no suite named '$n'${near:+ (did you mean: $near)}"
}
_t_run(){
  local a kit_args="" names="" stale_ok=0 f flags tmo cpu rc bad=0 n=0 out newer kitp
  for a in "$@"; do
    case "$a" in
      --case=*|--list|--bail|--verbose|--json) kit_args="$kit_args $a" ;;
      --stale-ok) stale_ok=1 ;;
      -*) die "t: unknown option '$a' (options: --case=PAT --list --bail --verbose --json --stale-ok)" ;;
      *) names="$names $a" ;;
    esac
  done
  [ -n "$names" ] || die "usage: t SUITE... [--case=PAT] [--list] [--bail] [--verbose] [--json]
       SUITE is a path, a name under tests/ (csv -> tests/test_csv.js) or mod:<module>"
  _require_dynajs
  newer=$(find "$SRC_DIR" repl.c -newer ./dynajs \( -name '*.c' -o -name '*.h' \) 2>/dev/null | head -3 | tr '\n' ' ')
  if [ -n "$newer" ] && [ "$stale_ok" != 1 ]; then
    die "t: ./dynajs is OLDER than ${newer}-- it does not contain those edits.
      rebuild first (./build.sh build <the config you are testing>), or pass
      --stale-ok when the newer file cannot affect this suite."
  fi
  T_FILES=""
  for a in $names; do _t_resolve "$a"; done
  [ -x tools/bounded-run.sh ] || die "t: tools/bounded-run.sh is missing; refusing to run unbounded"
  out=$(mktemp)
  for f in $T_FILES; do
    n=$((n + 1))
    kitp=0; grep -qE "from [\"']\.{1,2}/(\.\./)?kit\.js[\"']" "$f" 2>/dev/null && kitp=1
    if [ -n "$kit_args" ] && [ "$kitp" != 1 ]; then
      case "$kit_args" in *--case=*|*--list*)
        echo "FAIL: t: $f is not a kit suite (it does not import tests/kit.js), so it has no named cases to select" >&2
        bad=$((bad + 1)); continue ;;
      esac
    fi
    flags=$(head -3 "$f" | sed -n 's|^// *flags: *||p' | head -1)
    tmo=$(head -3 "$f" | sed -n 's|^// *timeout: *\([0-9][0-9]*\).*|\1|p' | head -1)
    [ -n "$tmo" ] || tmo=${DEV_SUITE_TIMEOUT:-120}
    cpu=$((tmo / 2)); [ "$cpu" -lt 1 ] && cpu=1
    echo "--- $f${kit_args:+ $kit_args}"
    # shellcheck disable=SC2086
    tools/bounded-run.sh "$tmo" "$cpu" "$f" -- ./dynajs $flags "$f" $([ "$kitp" = 1 ] && printf '%s' "$kit_args") </dev/null >"$out" 2>&1
    rc=$?
    cat "$out"
    if [ "$rc" = 124 ] || [ "$rc" = 125 ]; then
      echo "TIMEOUT: $f (bound ${tmo}s wall / ${cpu}s cpu, rc=$rc)" >&2; bad=$((bad + 1))
    elif [ "$rc" != 0 ]; then
      echo "FAIL: $f exited rc=$rc" >&2; bad=$((bad + 1))
    elif grep -qE '^[[:space:]]*FAIL([: ]|$)' "$out"; then
      echo "FAIL: $f exited 0 but printed a FAIL line" >&2; bad=$((bad + 1))
    elif [ ! -s "$out" ]; then
      echo "FAIL: $f exited 0 having printed nothing" >&2; bad=$((bad + 1))
    fi
  done
  rm -f "$out"
  [ "$bad" = 0 ] || die "t: $bad of $n suite(s) failed"
  echo "t: ok ($n suite(s))"
}
_require_dynajs(){
  [ -x ./dynajs ] || die "${cmd:-run}: no ./dynajs. Build one first: ./build.sh build"
}
# config CONTEXT only (parse + detect + object inventory): enough for the
# probe-first suite legs to know ENG_EXE/ENG_BUILD_SHARED/obj layout without
# compiling anything or touching stamps
_config_ctx(){
  eng_parse_cfg >/dev/null 2>&1 || true
  eng_detect
  eng_objects
}
_require_native_bin(){
  _require_dynajs
  ./dynajs -e 'import("dyna:mathx")' >/dev/null 2>&1 || {
    echo "FAIL: ./dynajs cannot load dyna:* modules."
    echo "      Build one first: ./build.sh build CONFIG_NATIVE_MODULES=y"
    exit 1
  }
}
_run_parallel(){  # $@ = suite files; fans out over ./tools/run-tests-parallel.sh
  [ $# -gt 0 ] || return 0
  DEV_JOBS=${DEV_JOBS:-$NCPU} ./tools/run-tests-parallel.sh "$@"
}

# ============================================================================
# Suite runners
# ============================================================================

# The engine-core JS suites (the recipe of the old default target, minus the
# example/bjson legs that only `test` carries): test_builtin first, the
# CORE_TESTS table on the parallel runner, then the --std handlers/async legs.
_core_tests_run(){
  local list="${CORE_LIST:-$CORE_TESTS_TABLE}"
  echo "core: tests/test_builtin.js first, then $(printf '%s' "$list" | wc -w | tr -d ' ') CORE suites"
  ./dynajs --std tests/test_builtin.js || { echo "FAIL: tests/test_builtin.js"; return 1; }
  _run_parallel $list || { echo "FAIL: core tests"; return 1; }
  ./dynajs --std tests/test_std.js        || { echo "FAIL: tests/test_std.js"; return 1; }
  ./dynajs --std tests/test_rw_handler.js || { echo "FAIL: tests/test_rw_handler.js"; return 1; }
  ./dynajs --std tests/test_async_api.js  || { echo "FAIL: tests/test_async_api.js"; return 1; }
  ./dynajs --std tests/test_async_leak.js || { echo "FAIL: tests/test_async_leak.js"; return 1; }
  return 0
}

# the full core-test recipe: suites + the three compiled examples +
# the bjson/point shared-module legs (only when this config builds .so).
_test_recipe(){
  _core_tests_run || return 1
  for e in examples/hello examples/hello_module examples/test_fib; do
    [ -x "$e" ] || { echo "FAIL: $e was not built (the default build produces it)"; return 1; }
    "./$e" || { echo "FAIL: $e"; return 1; }
  done
  echo "examples: ok"
  if [ "${ENG_BUILD_SHARED:-0}" = 1 ]; then
    _bjson_so || return 1
    ./dynajs tests/test_bjson.js   || { echo "FAIL: tests/test_bjson.js"; return 1; }
    ./dynajs examples/test_point.js || { echo "FAIL: examples/test_point.js"; return 1; }
  fi
  return 0
}

# The wide native matrix. Builds the config it runs against (the old target
# refused to rebuild for fear of silently swapping in a default-config binary;
# building the exact native config here closes that hole instead of hoping for
# it), probes dyna:* loading, runs the scoped/full list, then the fixed legs:
# log-format capture, the term review corpus, both base58 allocation gates,
# and test-examples (which chains check-readme -> check-install -> check-api).
_test_native_run(){
  _split_run_args "$@"
  _native_scope_list "$P2_SCOPE"
  if [ -n "$P2_CFG" ]; then
    # shellcheck disable=SC2086
    engine_build $P2_CFG || exit 1
  else
    engine_build $NATIVE_CFG || exit 1
  fi
  eng_objects
  _require_native_bin
  _park_sweep_so || exit 1
  echo "test-native: $(printf '%s' "$NATIVE_LIST" | wc -w | tr -d ' ') suites (TEST_SCOPE/MODULES scoped list)"
  _run_parallel $NATIVE_LIST || { echo "FAIL: test-native"; exit 1; }
  # dyna:log writes to stderr, which a process cannot read back: this one
  # captures fd 2 in a subshell and asserts the line FORMAT.
  sh tests/test_log_format.sh ./dynajs || { echo "FAIL: tests/test_log_format.sh"; exit 1; }
  # the terminal-module review corpus as permanent regression rows (its python
  # drivers SKIP without python3; the JS rows always run)
  if have python3; then
    sh tests/test_review_term.sh ./dynajs || { echo "FAIL: tests/test_review_term.sh"; exit 1; }
  else
    echo "=== SKIP tests/test_review_term.sh: python3 not found"
  fi
  # the base58 byte paths' allocation gates: exact-count rows, not tolerances
  tests/run_base58_alloc.sh ./dynajs  || { echo "FAIL: tests/run_base58_alloc.sh"; exit 1; }
  tests/run_base58_strict.sh ./dynajs || { echo "FAIL: tests/run_base58_strict.sh"; exit 1; }
  _test_examples_run || exit 1
  echo "test-native: ok"
}

# The dyna:* examples are RUN, not just shipped -- an example nobody executes
# is documentation nobody proofreads. Chains into check-readme (and from there
# the install/API gates), exactly as the old recipes did.
_test_examples_run(){
  _require_native_bin
  local e fl
  for e in examples/js/dynajs_*.js; do
    [ -f "$e" ] || continue
    fl=$(head -3 "$e" | grep -E '^// *flags:' | head -1 | sed -E 's|^//[[:space:]]*flags:||')
    # shellcheck disable=SC2086
    ./dynajs $fl "$e" >/dev/null || { echo "FAIL: $e"; return 1; }
    echo "  ok  $e"
  done
  echo "test-examples: every dyna:* example runs"
  _check_readme_run || return 1
  return 0
}

# ============================================================================
# Standalone C regression targets -- ported verbatim-in-effect: same sources,
# same flags, same run as the recipes they replace. None of them link the
# configured engine; they compile hand-named sources with the plain host
# compiler and their own sanitizer flags, exactly as before.
# ============================================================================

_asan_ubsan_O1(){  # convenience: the shared sanitizer prefix
  printf '%s' "-g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer"
}
_docker_uring(){  # $1 = name, $2 = test source, $3 = out name, $4.. extra defines
  local name="$1" src="$2" out="$3"; shift 3
  if ! docker info >/dev/null 2>&1; then
    echo "SKIP (not run here): $name needs docker (Linux-only io_uring backend)"
    return 0
  fi
  docker run --rm --security-opt seccomp=unconfined -v "$ROOT:/src" -w /src dynajs:deps bash -c "
    cc -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
     -std=gnu17 -D_GNU_SOURCE -DCONFIG_NATIVE_MODULES -DCONFIG_IO_URING $* \
     -Isrc -Isrc/core $src src/dyna-aio-uring.c \
     src/dyna-evloop.c src/dyna-io.c src/core/dyn-pool.c src/cutils.c \
     -lpthread -luring -o /tmp/$out \
     && /tmp/$out"
}

_c_regression_targets(){
case "$1" in
  test-io-atomic)
    mkdir -p .obj
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
      -Wall -Wextra -std=gnu17 -Isrc \
      tests/test_io_atomic_write.c src/dyna-io.c src/cutils.c -o .obj/test_io_atomic || return 1
    ./.obj/test_io_atomic ;;
  test-io-slurp-cap)
    mkdir -p .obj
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
      -Wall -Wextra -std=gnu17 -Isrc \
      tests/test_io_slurp_cap.c src/dyna-io.c src/cutils.c -o .obj/test_io_slurp_cap || return 1
    ./.obj/test_io_slurp_cap ;;
  test-regexp-prefilter)
    mkdir -p .obj
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
      -Wall -Wextra -Wno-unused-parameter -std=gnu17 -I. -Isrc \
      tests/test_regexp_prefilter.c src/libregexp.c src/cutils.c src/libunicode.c \
      $SIMD_SRCS -o .obj/test_regexp_prefilter || return 1
    ASAN_OPTIONS=detect_leaks=0 ./.obj/test_regexp_prefilter ;;
  test-dtoa-subnormal)
    mkdir -p .obj
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
      -Wall -Wextra -Wno-unused-parameter -std=gnu17 -I. -Isrc \
      tests/test_dtoa_subnormal.c src/dtoa.c src/cutils.c -o .obj/test_dtoa_subnormal || return 1
    ASAN_OPTIONS=detect_leaks=0 ./.obj/test_dtoa_subnormal ;;
  test-simd-bitmap)
    mkdir -p .obj
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
      -Wall -Wextra -Wno-unused-parameter -std=gnu17 -I. -Isrc \
      tests/test_simd_bitmap.c $SIMD_SRCS src/cutils.c -o .obj/test_simd_bitmap || return 1
    ASAN_OPTIONS=detect_leaks=0 ./.obj/test_simd_bitmap ;;
  # The SIMD/text kernel proofs had NO target at all (B1-08): the .c files sat
  # in tests/ with nothing in build.sh naming them, so they ran in no gate and
  # under no manual subcommand. Self-contained: kernels + cutils only.
  test-simd-f64|test-simd-int|test-simd-reductions|test-text-kernels|test-utf16-kernels|test-simd-tiers)
    mkdir -p .obj
    local kern="test_${1#test-}"; kern="${kern//-/_}"   # dashed name -> file stem
    # shellcheck disable=SC2086
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
      -Wall -Wextra -Wno-unused-parameter -std=gnu17 -I. -Isrc \
      "tests/${kern}.c" $SIMD_SRCS src/cutils.c -lm -o ".obj/$1" || return 1
    ASAN_OPTIONS=detect_leaks=0 ".obj/$1" ;;
  # The x86 tiers, EXECUTED on an Apple-silicon Mac. Code for an instruction
  # set this machine cannot run is unrun, not tested -- but Rosetta 2 runs
  # x86-64 here, advertising SSE4.2 by default and AVX2+FMA when
  # ROSETTA_ADVERTISE_AVX=1. So every kernel harness is built for x86_64 and
  # run twice, once per tier, under ASan+UBSan; each harness prints the tier
  # that actually ran and that line is checked, so a harness that silently
  # fell back to another tier fails instead of passing. Elsewhere (Linux, an
  # Intel Mac, no Rosetta) this is a LOUD skip: those hosts run their own tier
  # through the ordinary targets.
  test-simd-x86)
    if [ "$(uname -s)" != Darwin ] || [ "$(uname -m)" != arm64 ] || ! arch -x86_64 /usr/bin/true 2>/dev/null; then
      echo "test-simd-x86: SKIP -- needs Rosetta 2 on an Apple-silicon Mac (this host runs its own tier via test-simd-*)"
      return 0
    fi
    mkdir -p .obj/x86
    local h mode out rc want
    for h in test_simd_tiers test_simd_f64 test_simd_int test_simd_reductions test_text_kernels test_utf16_kernels test_simd_bitmap; do
      # shellcheck disable=SC2086
      clang -arch x86_64 -w -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
        -std=gnu17 -I. -Isrc \
        "tests/$h.c" $SIMD_SRCS src/cutils.c -lm -o ".obj/x86/$h" || { echo "FAIL: test-simd-x86: $h does not build for x86_64"; return 1; }
      for mode in sse4.2 avx2; do
        if [ "$mode" = avx2 ]; then
          out=$(ROSETTA_ADVERTISE_AVX=1 ASAN_OPTIONS=detect_leaks=0 ".obj/x86/$h" 2>&1); rc=$?
          want='avx2=1|AVX2=1|tier: avx2'
        else
          out=$(ASAN_OPTIONS=detect_leaks=0 ".obj/x86/$h" 2>&1); rc=$?
          want='avx2=0|AVX2=0|tier: sse4.2'
        fi
        if [ "$rc" != 0 ]; then
          printf '%s\n' "$out" | tail -15
          echo "FAIL: test-simd-x86: $h on the $mode tier (rc=$rc)"; return 1
        fi
        printf '%s\n' "$out" | grep -qE "$want" || {
          printf '%s\n' "$out" | head -3
          echo "FAIL: test-simd-x86: $h did not run on the $mode tier (its ISA line says otherwise)"; return 1; }
        echo "  $h [$mode]: ok"
      done
    done ;;
  # CLOEXEC / descriptor-inheritance proofs (B1-16: "exist, no build target").
  # dyna-aio.h and dyna-evloop.h only declare their types under
  # CONFIG_NATIVE_MODULES, so a target without that define cannot even compile.
  test-socket-cloexec)
    mkdir -p .obj
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
      -Wall -Wextra -Wno-unused-parameter -std=gnu17 -D_GNU_SOURCE \
      -DCONFIG_NATIVE_MODULES -I. -Isrc -Isrc/core \
      tests/test_socket_cloexec.c src/dyna-aio.c src/dyna-evloop.c src/dyna-io.c \
      src/core/dyn-pool.c src/core/dyn-timer.c src/core/dyn-prng.c src/cutils.c \
      -lpthread -o .obj/test_socket_cloexec || return 1
    ASAN_OPTIONS=detect_leaks=0 ./.obj/test_socket_cloexec ;;
  test-evloop-cloexec)
    mkdir -p .obj
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
      -Wall -Wextra -Wno-unused-parameter -std=gnu17 -D_GNU_SOURCE \
      -DCONFIG_NATIVE_MODULES -I. -Isrc -Isrc/core \
      tests/test_evloop_cloexec.c src/dyna-evloop.c src/dyna-io.c \
      src/core/dyn-pool.c src/core/dyn-timer.c src/core/dyn-prng.c src/cutils.c \
      -lpthread -o .obj/test_evloop_cloexec || return 1
    ASAN_OPTIONS=detect_leaks=0 ./.obj/test_evloop_cloexec ;;
  test-ds-core)
    mkdir -p .obj
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
      -Wall -Wextra -std=gnu17 -Isrc/core \
      tests/test_ds_core.c src/core/dyn-ds.c src/core/dyn-hash.c -o .obj/test_ds_core || return 1
    ./.obj/test_ds_core ;;
  test-ac-caps)
    mkdir -p .obj
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
      -Wall -Wextra -std=gnu17 -Isrc/core \
      tests/test_ac_caps.c src/core/dyn-ac.c -o .obj/test_ac_caps || return 1
    ./.obj/test_ac_caps ;;
  test-ds-btree-oom)
    mkdir -p .obj
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
      -Wall -Wextra -std=gnu17 -D_GNU_SOURCE -Isrc/core \
      tests/test_ds_btree_oom.c src/core/dyn-ds.c src/core/dyn-hash.c \
      -o .obj/test_ds_btree_oom || return 1
    ./.obj/test_ds_btree_oom ;;
  test-pool)
    mkdir -p .obj
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
      -Wall -Wextra -std=gnu17 -Isrc/core \
      tests/test_pool.c src/core/dyn-pool.c -lpthread -o .obj/test_pool || return 1
    ./.obj/test_pool || return 1
    $(reg_cc) -g -O1 -fsanitize=thread -fno-omit-frame-pointer \
      -Wall -Wextra -std=gnu17 -Isrc/core \
      tests/test_pool.c src/core/dyn-pool.c -lpthread -o .obj/test_pool_tsan || return 1
    ./.obj/test_pool_tsan ;;
  test-dns-codec)
    mkdir -p .obj
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
      -Wall -Wextra -std=gnu17 -Isrc/core \
      tests/test_dns_codec.c src/core/dyn-dns.c -o .obj/test_dns_codec || return 1
    ./.obj/test_dns_codec ;;
  test-resp-codec)
    # -Isrc and dtoa.c: dyn-resp.c uses js_atod for a correctly-rounded,
    # locale-free float parse. A standalone target inherits NONE of the main
    # build's include paths or objects, so it must name them itself.
    mkdir -p .obj
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
      -Wall -Wextra -std=gnu17 -Isrc/core -Isrc \
      tests/test_resp_codec.c src/core/dyn-resp.c src/dtoa.c src/cutils.c \
      -o .obj/test_resp_codec -lm || return 1
    ./.obj/test_resp_codec ;;
  test-scram)
    # the dyn-simd-stub.c the first shape once compiled is gone, so the first
    # attempt fails silently and the SIMD-sources shape runs -- same as before
    mkdir -p .obj
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
      -Wall -Wextra -std=gnu17 -Isrc/core \
      tests/test_scram.c src/core/dyn-scram.c src/core/dyn-hash.c \
      src/core/dyn-codec.c src/core/dyn-prng.c src/core/dyn-simd-stub.c \
      -o .obj/test_scram 2>/dev/null || \
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
      -Wall -Wextra -std=gnu17 -Isrc/core -Isrc \
      tests/test_scram.c src/core/dyn-scram.c src/core/dyn-hash.c \
      src/core/dyn-codec.c src/core/dyn-prng.c $SIMD_SRCS \
      -o .obj/test_scram || return 1
    ./.obj/test_scram ;;
  test-timer)
    mkdir -p .obj
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
      -Wall -Wextra -std=gnu17 -Isrc/core \
      tests/test_timer.c src/core/dyn-timer.c -o .obj/test_timer || return 1
    ./.obj/test_timer ;;
  test-bc-read-safety)
    mkdir -p .obj
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
      -std=gnu17 -D_GNU_SOURCE -DCONFIG_VERSION=\"x\" -I. -Isrc -Isrc/core \
      tests/test_bc_read_safety.c src/dynajs.c src/engine-parser.c \
      src/engine-serialize.c src/dtoa.c src/cutils.c \
      src/libregexp.c src/libunicode.c src/dyna-io.c \
      src/dyna-simd-core.c src/dyna-simd-scalar.c src/dyna-simd-neon.c \
      src/dyna-simd-sse42.c src/dyna-simd-avx2.c src/dyna-simd-avx512.c \
      src/dyna-simd-sve.c src/core/*.c third_party/openlibm/libopenlibm.a \
      -lm -lpthread -ldl -o .obj/test_bc_read_safety || return 1
    ./.obj/test_bc_read_safety ;;
  test-stack-limit)
    mkdir -p .obj
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
      -std=gnu17 -D_GNU_SOURCE -DCONFIG_VERSION=\"x\" -I. -Isrc -Isrc/core \
      tests/test_stack_limit.c src/dynajs.c src/engine-parser.c \
      src/engine-serialize.c src/dtoa.c src/cutils.c \
      src/libregexp.c src/libunicode.c src/dyna-io.c \
      src/dyna-simd-core.c src/dyna-simd-scalar.c src/dyna-simd-neon.c \
      src/dyna-simd-sse42.c src/dyna-simd-avx2.c src/dyna-simd-avx512.c \
      src/dyna-simd-sve.c src/core/*.c third_party/openlibm/libopenlibm.a \
      -lm -lpthread -ldl -o .obj/test_stack_limit || return 1
    ./.obj/test_stack_limit ;;
  test-aio-sendfile-busy)
    mkdir -p .obj
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
      -std=gnu17 -D_GNU_SOURCE -DCONFIG_NATIVE_MODULES -Isrc -Isrc/core \
      tests/test_aio_sendfile_busy.c src/dyna-aio.c src/dyna-evloop.c \
      src/dyna-io.c src/core/dyn-pool.c src/cutils.c -lpthread \
      -o .obj/test_aio_sendfile_busy || return 1
    ./.obj/test_aio_sendfile_busy ;;
  test-aio-connect-offload)
    mkdir -p .obj
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
      -std=gnu17 -D_GNU_SOURCE -DCONFIG_NATIVE_MODULES -Isrc -Isrc/core \
      tests/test_aio_connect_offload.c src/dyna-aio.c src/dyna-evloop.c \
      src/dyna-io.c src/core/dyn-pool.c src/cutils.c -lpthread \
      -o .obj/test_aio_connect_offload || return 1
    ./.obj/test_aio_connect_offload ;;
  test-dns-foreign-answers)
    mkdir -p .obj
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
      -std=gnu17 -D_GNU_SOURCE -DCONFIG_NATIVE_MODULES \
      -DCONFIG_NATIVE_MODULE_NET -I. -Isrc -Isrc/core \
      tests/test_dns_foreign_answers.c src/dyna-aio.c src/dyna-evloop.c \
      src/dyna-io.c src/core/dyn-pool.c src/core/dyn-timer.c \
      src/core/dyn-prng.c src/core/dyn-dns.c src/cutils.c -lpthread \
      -o .obj/test_dns_foreign_answers || return 1
    ./.obj/test_dns_foreign_answers ;;
  test-aio-disk)
    mkdir -p .obj
    $(reg_cc) -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
      -std=gnu17 -D_GNU_SOURCE -DCONFIG_NATIVE_MODULES -Isrc -Isrc/core \
      tests/test_aio_disk.c src/dyna-aio.c src/dyna-evloop.c \
      src/dyna-io.c src/core/dyn-pool.c src/cutils.c -lpthread -o .obj/test_aio_disk || return 1
    ./.obj/test_aio_disk || return 1
    $(reg_cc) -g -O1 -fsanitize=thread -fno-omit-frame-pointer \
      -std=gnu17 -D_GNU_SOURCE -DCONFIG_NATIVE_MODULES -Isrc -Isrc/core \
      tests/test_aio_disk.c src/dyna-aio.c src/dyna-evloop.c \
      src/dyna-io.c src/core/dyn-pool.c src/cutils.c -lpthread -o .obj/test_aio_disk_tsan || return 1
    ./.obj/test_aio_disk_tsan ;;
  test-exec-spawn)
    mkdir -p .obj
    { \
      echo '#include <errno.h>'; \
      echo '#include <stdlib.h>'; \
      echo '#include <fcntl.h>'; \
      echo '#include <limits.h>'; \
      echo '#include <spawn.h>'; \
      echo '#include <stddef.h>'; \
      echo '#include <string.h>'; \
      echo '#include <sys/types.h>'; \
      echo '#include <sys/wait.h>'; \
      echo '#include <unistd.h>'; \
      sed -n '/^#define DYN_SPAWN_EXTRACT_BEGIN/,/^#define DYN_SPAWN_EXTRACT_END/p' \
        src/dyna-libc.c; } \
      | sed -e '/^#define DYN_SPAWN_EXTRACT/d' \
            -e 's/^static int os_exec_spawn_child(/int os_exec_spawn_child(/' \
      > .obj/spawn_impl.c
    $(reg_cc) -g -O1 -std=gnu17 -D_GNU_SOURCE -Wno-deprecated-declarations \
      tests/test_exec_spawn.c .obj/spawn_impl.c -lpthread \
      -o .obj/test_exec_spawn || return 1
    ./.obj/test_exec_spawn ;;
  test-uring-sendfile)
    _docker_uring test-uring-sendfile tests/test_uring_sendfile.c test_uring_sendfile ;;
  test-uring-connect-offload)
    _docker_uring test-uring-connect-offload tests/test_aio_connect_offload.c test_uring_connect ;;
  test-uring-tick)
    _docker_uring test-uring-tick tests/test_aio_uring_timer.c test_uring_tick ;;
  test-crc32c-hw)
    mkdir -p .obj
    $(reg_cc) -O2 -std=gnu17 -Isrc/core -Isrc -Wall -Wextra \
      tests/test_crc32c_hw.c src/core/dyn-hash.c $SIMD_SRCS -lm \
      -o .obj/test_crc32c_hw || return 1
    ./.obj/test_crc32c_hw ;;
  test-crc32c-race)
    mkdir -p .obj
    $(reg_cc) -O1 -g -std=gnu17 -fsanitize=thread -fno-omit-frame-pointer \
      -Isrc/core -Isrc -Wall -Wextra \
      tests/test_crc32c_race.c src/core/dyn-hash.c $SIMD_SRCS -lm \
      -o .obj/test_crc32c_race || return 1
    ./.obj/test_crc32c_race ;;
  test-sha256-hw)
    mkdir -p .obj
    $(reg_cc) -O2 -std=gnu17 -Isrc/core -Isrc -Wall -Wextra \
      tests/test_sha256_hw.c src/core/dyn-hash.c $SIMD_SRCS -lm \
      -o .obj/test_sha256_hw || return 1
    $(reg_cc) -O2 -std=gnu17 -Isrc/core -Isrc -Wall -Wextra -DDYN_SHA256_NO_HW=1 \
      tests/test_sha256_hw.c src/core/dyn-hash.c $SIMD_SRCS -lm \
      -o .obj/test_sha256_sw || return 1
    echo "  hardware:"; ./.obj/test_sha256_hw || return 1
    echo "  scalar:  "; ./.obj/test_sha256_sw ;;
  test-aio-tls)
    # links the CONFIGURED engine (its recipe took the build config's CFLAGS,
    # NAT_MODULE_OBJS and libdynajs.a), so the objects must exist and be fresh
    [ -n "$ENG_LIB_OBJS" ] || { echo "FAIL: test-aio-tls needs the configured build objects"; return 1; }
    mkdir -p .obj
    local objs=""
    local o
    for o in $ENG_NAT_OBJS; do objs="$objs $OBJDIR/$o.o"; done
    # shellcheck disable=SC2086
    $ENG_CC -O1 -g $ENG_CFLAGS -Isrc -o .obj/test_aio_tls tests/test_aio_tls.c \
      $objs $ROOT/libdynajs.a $ENG_LDFLAGS $ENG_LIBS ${ENG_SQLITE_LIBS:-} || return 1
    ./.obj/test_aio_tls ;;
  test-net-final-sweep|test-net-udp-grave)
    # links the engine minus the CLI/REPL entry objects, under the build config
    [ -f "$OBJDIR/dyna-net.o" ] || { echo "SKIP: $1 needs a CONFIG_NATIVE_MODULES=y build"; exit 1; }
    mkdir -p .obj
    local out exe srcs skip="" o bin   # skip MUST be initialized: set -u
    if [ "$1" = test-net-final-sweep ]; then
      out="$OBJDIR/test_net_final_sweep"; srcs="tests/test_net_final_sweep.c"; bin="$out"
    else
      [ -f "$OBJDIR/dyna-net-tcp.o" ] || { echo "SKIP: test-net-udp-grave needs a CONFIG_NATIVE_MODULES=y build"; exit 1; }
      out="$OBJDIR/test_udp_grave_sweep"; srcs="tests/test_udp_grave_sweep.c"; bin="$out"
    fi
    objs=""
    for o in dyna-cli repl; do skip="$skip $OBJDIR/$o.o"; done
    for o in $ENG_LIB_OBJS $ENG_NAT_OBJS; do
      case " $skip " in *" $OBJDIR/$o.o "*) continue ;; esac
      objs="$objs $OBJDIR/$o.o"
    done
    # shellcheck disable=SC2086
    $ENG_CC $ENG_CFLAGS $ENG_LDFLAGS $ENG_LDEXPORT -o "$out" $srcs $objs $ENG_LIBS || return 1
    local marker=".obj/.${1}.bounded" leak=""
    [ "$1" = test-net-udp-grave ] && leak="ASAN_OPTIONS=detect_leaks=1 "
    rm -f .obj/."${1}".bounded
    # shellcheck disable=SC2086
    eval "${leak}BOUNDED_RUN_MARKER=$marker ./tools/bounded-run.sh 120 60 \"$1\" -- \"$bin\"" \
      > ".obj/${1}.out" 2>&1
    local rc=$?
    cat ".obj/${1}.out"
    if [ "$rc" -ne 0 ]; then
      if [ -f "$marker" ]; then
        rm -f "$marker"
        echo "TIMEOUT: $1 hit the external bound (exit $rc)"; exit "$rc"
      fi
      echo "FAIL: $1 exited $rc (a row failed, or a sanitizer reported -- see the rows above)"; exit 1
    fi
    rm -f "$marker"
    grep -q "fails=0" ".obj/${1}.out" || { echo "FAIL: $1 found a regression (see the rows above)"; exit 1; } ;;
  test-net-fdchurn|test-net-udp-selfclose|test-net-teardown-churn|test-net-eyeballs-churn)
    _require_native_bin
    case "$1" in
      test-net-fdchurn)
        mkdir -p .obj; rm -f .obj/.test-net-fdchurn.bounded
        BOUNDED_RUN_MARKER=.obj/.test-net-fdchurn.bounded \
          ./tools/bounded-run.sh 120 60 "test-net-fdchurn" -- \
          sh -c 'ulimit -n 64; exec ./dynajs --std tests/test_net_fdchurn.js' \
          > .obj/test-net-fdchurn.out 2>&1
        local rc=$?
        cat .obj/test-net-fdchurn.out
        if [ "$rc" -ne 0 ] && [ -f .obj/.test-net-fdchurn.bounded ]; then
          rm -f .obj/.test-net-fdchurn.bounded
          echo "TIMEOUT: test-net-fdchurn hit the external bound (exit $rc)"; exit "$rc"
        fi
        rm -f .obj/.test-net-fdchurn.bounded
        grep -q "errors=0" .obj/test-net-fdchurn.out \
          || { echo "FAIL: descriptors leak across the cycle (or a cycle stalled -- see the watchdog diagnostic above)"; exit 1; } ;;
      test-net-udp-selfclose)
        ./dynajs --std tests/test_net_udp_selfclose.js | tee /dev/stderr | \
          grep -q "fails=0" || { echo "FAIL: UDPSocket self-close reads freed state (see the scenario above)"; exit 1; } ;;
      test-net-teardown-churn)
        ./dynajs --std tests/test_net_teardown_churn.js 25 | tee /dev/stderr | \
          grep -q "fails=0" || { echo "FAIL: a teardown path leaked, crashed, or exited through the wrong door"; exit 1; } ;;
      test-net-eyeballs-churn)
        ( ulimit -n 64; ./dynajs tests/test_net_eyeballs.js 120 ) | tee /dev/stderr | \
          grep -q "errors=0" || { echo "FAIL: the race loser leaks a descriptor"; exit 1; } ;;
    esac ;;
  bench-core-tus)
    mkdir -p .obj
    $(reg_cc) -O2 -std=gnu17 -I src/core -I src -Wall -Wextra -Wno-unused-parameter \
      tests/bench_core.c src/core/dyn-ds.c src/core/dyn-codec.c src/core/dyn-path.c \
      src/core/dyn-prng.c src/core/dyn-ac.c src/core/dyn-serial.c src/core/dyn-hash.c \
      src/core/dyn-mathx.c src/core/dyn-dict.c src/core/dyn-compress.c \
      $SIMD_SRCS src/cutils.c -lm -o .obj/bench_core || return 1
    ./.obj/bench_core ;;
  bench-ac)
    mkdir -p .obj
    $(reg_cc) -O2 -std=gnu17 -Isrc/core -Wall -Wextra \
      tests/bench_ac.c src/core/dyn-ac.c -o .obj/bench_ac || return 1
    ./.obj/bench_ac ;;
  *)
    return 99 ;;
esac
}

# the SIMD source set the standalone targets name by hand (same list)
SIMD_SRCS="src/dyna-simd-core.c src/dyna-simd-scalar.c src/dyna-simd-neon.c \
src/dyna-simd-sse42.c src/dyna-simd-avx2.c src/dyna-simd-avx512.c src/dyna-simd-sve.c"

# pure-JS single-suite runners: run against the binary that is already there,
# NEVER rebuilding (a rebuild in the wrong config silently turns every dyna:*
# case into a green skip). The probe is the failure mode.
_js_suite(){
  _require_dynajs
  local t="$1"; shift
  # shellcheck disable=SC2086
  _v ./dynajs "$@" || { echo "FAIL: $cmd: ./dynajs $* $t exited nonzero"; exit 1; }
  echo "$cmd: ok"
}

# ============================================================================
# check-* documentation/install gates. The chain is preserved: test-examples
# -> check-readme -> check-install -> check-api -> {check-anchors,
# check-error-ids, check-types} -> check-dts-truth. Each is callable alone.
# ============================================================================
_check_readme_run(){
  if [ -f README.md ]; then
    ./tools/check-readme-examples.sh README.md ./dynajs || { echo "FAIL: check-readme"; return 1; }
  else
    echo "check-readme: SKIPPED -- README.md absent"
  fi
  _check_install_run || return 1
  echo "check-readme: ok"
}
_check_install_run(){
  ./tests/test_install_brew.sh ./install.sh || { echo "FAIL: check-install (brew bootstrap tests)"; return 1; }
  ./tests/test_install_flow.sh ./install.sh || { echo "FAIL: check-install (flow tests)"; return 1; }
  _check_api_run || return 1
  echo "check-install: ok"
}
_check_api_run(){
  ./tools/check-apps.sh examples/apps ./dynajs || { echo "FAIL: check-api (examples/apps)"; return 1; }
  _check_anchors_run   || return 1
  _check_error_ids_run || return 1
  _check_types_run     || return 1
  echo "check-api: ok"
}
_check_anchors_run(){
  if [ -f README.md ]; then
    python3 tools/check-anchors.py README.md \
      && echo "check-anchors: every internal link resolves" \
      || { echo "FAIL: check-anchors"; return 1; }
  else
    echo "check-anchors: SKIPPED -- README.md absent"
  fi
}
_check_error_ids_run(){
  [ -x ./dynajs ] || { echo "FAIL: check-error-ids needs ./dynajs -- build first"; return 1; }
  OBJDIR="${OBJDIR:-.obj}" python3 tools/check-error-identifiers.py ./dynajs \
    || { echo "FAIL: check-error-ids"; return 1; }
}
_check_types_run(){
  python3 tools/check-dts-coverage.py || { echo "FAIL: dynajs.d.ts has drifted from the binary"; return 1; }
  if have tsc; then
    tsc --noEmit --strict --lib es2023 dynajs.d.ts || { echo "FAIL: tsc dynajs.d.ts"; return 1; }
  else
    echo "check-types: SKIPPED -- tsc not installed (d.ts coverage verified, typecheck not)"
  fi
  _check_dts_truth_run || return 1
  echo "check-types: ok"
}
_check_dts_truth_run(){
  _require_dynajs
  python3 tools/check-dts-truth.py --dynajs ./dynajs || { echo "FAIL: check-dts-truth"; return 1; }
}

# every tests/test_*.js named on a non-comment line of build.sh must land in
# the evaluated universe: a module table's list (for the modules the runners
# actually enumerate), MULTI/STANDALONE/CORE/SECURITY/API, or a suite a recipe
# line runs directly. Catches the append-before-definition class: a table
# entry whose module nothing enumerates is an orphan no gate would run.
_check_test_list_run(){
  local missing="" universe t
  universe=" $CORE_TESTS_TABLE $SECURITY_TESTS_TABLE $API_TESTS_TABLE $MT_MULTI $MT_STANDALONE_TABLE "
  local m
  for m in $MT_SLIM_MODS $MT_GATEONLY_MODS; do
    mt_modtests "$m" >/dev/null 2>&1 && universe="$universe $(mt_modtests "$m") "
  done
  # suites a recipe line runs directly (./dynajs, run-tests-parallel, run.sh)
  universe="$universe $(grep -vE '^[[:space:]]*#' "$0" | grep -oE 'tests/[A-Za-z0-9_.-]*\.js' | sort -u | tr '\n' ' ') "
  for t in $(grep -vE '^[[:space:]]*#' "$0" | grep -oE 'tests/test_[A-Za-z0-9_.-]*\.js' | sort -u); do
    case " $universe " in *" $t "*) ;; *) missing="$missing $t" ;; esac
  done
  if [ -n "$missing" ]; then
    echo "FAIL: named in build.sh but runs in NO gate:$missing"
    echo "      (usually a table entry under a module name nothing enumerates)"
    return 1
  fi
  echo "check-test-list: every tests/test_*.js named in build.sh is gated"
}

_conformance_run(){
  local b f t
  b=$(sed -n 's/^BASELINE="\${T262_BASELINE:-\([0-9]*\/[0-9]*\)}".*/\1/p' build.sh)
  f=${b%%/*}; t=${b##*/}
  [ -n "$f" ] && [ -n "$t" ] || { echo "FAIL: no BASELINE in build.sh"; return 1; }
  printf 'test262: %s failures / %s tests (%.4f%% pass), pinned in build.sh\n' \
    "$f" "$t" "$(awk "BEGIN{printf \"%.4f\", (1-$f/$t)*100}")"
}

# __cc-line / __cc-lines: the exact compile commands the engine would run --
# the audits (tools/vecaudit.sh, tools/c17-audit.sh) consume these instead of
# re-deriving flags, so they cannot drift from the real build.
__cc_line_of(){  # $1 = object basename -> one compile line on stdout
  local src
  src=$(eng_src_of "$1") || { echo "__cc: no source for $1.c" >&2; return 1; }
  # the line is SHELL SOURCE (the audits re-parse it with sh/eval), so the
  # quotes around -DCONFIG_VERSION/-DCONFIG_CC must be escaped the way the
  # old dry-run output printed them -- at engine level they ride a variable
  # expansion where quotes are data, but re-parsed they would be stripped
  printf '%s\n' "$ENG_CC $ENG_CFLAGS_OPT -c -o $OBJDIR/$1.o $src" | sed 's/"/\\"/g'
}
_cc_lines_run(){
  local o
  eng_parse_cfg "$@" || return 1
  eng_detect
  eng_flags || return 1
  eng_objects
  __cc_line_of dynajsc || return 1
  __cc_line_of dyna-cli || return 1
  for o in $ENG_LIB_OBJS; do __cc_line_of "$o" || return 1; done
  for o in $ENG_NAT_OBJS; do __cc_line_of "$o" || return 1; done
  # the generated-code objects exist only after a build; skip them when the
  # generated sources are absent (a real build generates them first)
  [ -f repl.c ] && __cc_line_of repl
  return 0
}

# ============================================================================
# install / install-hooks
# ============================================================================
_install_run(){  # $@ = [PREFIX] [CONFIG_*...]; DESTDIR env honored
  local prefix="" a cfg=""
  for a in "$@"; do
    case "$a" in
      CONFIG_*) cfg="$cfg $a" ;;
      *) [ -n "$prefix" ] && die "install: extra argument: $a"; prefix="$a" ;;
    esac
  done
  [ -n "$prefix" ] || prefix="${PREFIX:-/usr/local}"
  local destdir="${DESTDIR:-}"
  # shellcheck disable=SC2086
  engine_build $HARDEN_CFG $cfg || exit 1
  local strip="${STRIP:-strip}"
  mkdir -p "$destdir$prefix/bin"
  $strip "${BIN_PFX}dynajs$ENG_EXE" "${BIN_PFX}dynajsc$ENG_EXE"
  install -m755 "${BIN_PFX}dynajs$ENG_EXE" "${BIN_PFX}dynajsc$ENG_EXE" "$destdir$prefix/bin"
  mkdir -p "$destdir$prefix/lib/dynajs"
  install -m644 "${BIN_PFX}libdynajs.a" "$destdir$prefix/lib/dynajs"
  if [ -n "$(cfg_v LTO)" ]; then
    install -m644 "${BIN_PFX}libdynajs.lto.a" "$destdir$prefix/lib/dynajs"
  fi
  mkdir -p "$destdir$prefix/include/dynajs"
  install -m644 src/dynajs.h src/dyna-libc.h "$destdir$prefix/include/dynajs"
  echo "install: ok ($destdir$prefix)"
}
_install_hooks_run(){
  [ -d .git ] || { echo "FAIL: not a git working tree, no hooks to install"; return 1; }
  mkdir -p .git/hooks
  if test -f .git/hooks/pre-push && \
      ! grep -q 'dynajs-prepush-hook' .git/hooks/pre-push 2>/dev/null; then
    echo "FAIL: .git/hooks/pre-push exists and is not ours. Refusing to overwrite."
    echo "      Move it aside, or add this line to it:  ./build.sh prepush"
    return 1
  fi
  cp tools/pre-push-hook.sh .git/hooks/pre-push && chmod +x .git/hooks/pre-push
  echo "install-hooks: pre-push installed (gates only when the pushed range touches a code path)"
}

# ============================================================================
# misc targets: stats, microbench, pgo, sbom, the test262 bootstrap/runners,
# test-nofile, test-uring, the oracles, bench-core
# ============================================================================
_pgo_run(){
  local train="tests/microbench.js tests/bench_array_ext.js"
  local extra=""
  [ -n "${CONFIG_MIMALLOC:-}" ] && extra="$extra CONFIG_MIMALLOC=y"
  [ -n "${CONFIG_LTO:-}" ] && extra="$extra CONFIG_LTO=y"
  local prof
  if have llvm-profdata; then prof="llvm-profdata"
  else prof="xcrun llvm-profdata"; fi
  ENG_NO_EXAMPLES=1 engine_build CONFIG_NATIVE_MODULES=y CONFIG_PGO_GEN=y $extra || exit 1
  rm -rf pgo-data pgo.profdata
  local t
  for t in $train; do
    LLVM_PROFILE_FILE="pgo-data/%p-%m.profraw" ./dynajs "$t" >/dev/null 2>&1 || true
  done
  $prof merge -o pgo.profdata pgo-data/*.profraw || exit 1
  engine_clean
  ENG_NO_EXAMPLES=1 engine_build CONFIG_NATIVE_MODULES=y CONFIG_PGO_USE=y $extra || exit 1
  echo "PGO build complete (trained on: $train)"
}
_test_nofile_run(){
  [ -f src/dyna-file.c ] || die "test-nofile: src/dyna-file.c already absent"
  # the hold file lives in this process's private scratch dir, NEVER at a fixed
  # /tmp path (a second concurrent run -- or a pre-planted symlink -- used to
  # lose or displace the tracked source; a crash between mv and restore simply
  # deleted it from the tree). restore_file is chained onto the build lock's
  # EXIT trap so an abort still puts the source back.
  local hold="$DEV_SCRATCH/dyna-file.c.hold" logf="$DEV_SCRATCH/nofile.log"
  mv src/dyna-file.c "$hold"
  restore_file(){ mv "$hold" src/dyna-file.c 2>/dev/null || true; eng_lock_release; }
  trap 'restore_file; tree_lock_release' EXIT
  engine_clean
  if ! engine_build CONFIG_NATIVE_MODULES=y >"$logf" 2>&1; then
    echo "FAIL: build without dyna:file"; tail -20 "$logf"; exit 1
  fi
  if ! ./dynajs -e 'import("dyna:csv").then(m=>{try{new m.CSVFile("x")}catch(e){
     if(!e.message.includes("dyna:file is not built in")) throw new Error("wrong message: "+e.message);
     print("  ok  the fallback names the real cause")}})'; then
    echo "FAIL: the no-dyna:file fallback misbehaved"; exit 1
  fi
  echo "test-nofile: the no-dyna:file fallback compiles, links and reports correctly"
  restore_file
  trap 'eng_lock_release; tree_lock_release' EXIT
  rm -rf "$DEV_SCRATCH"
  engine_clean
}
_test_uring_run(){
  docker build --platform linux/amd64 --target uring-bench -f docker/Dockerfile -t dynajs:uringtest . \
    || { echo "FAIL: docker build (uring-bench)"; exit 1; }
  # the suite declares `// flags: --std` in its header; the legacy recipe ran
  # it bare, which cannot even load std -- pass the declared flags
  docker run --rm --platform linux/amd64 dynajs:uringtest ./dynajs --std tests/test_uring_disk.js \
    || { echo "FAIL: uring disk test in container"; exit 1; }
  echo "test-uring: ok"
}
_test2_bootstrap_run(){
  local commit="5c8206929d81b2d3d727ca6aac56c18358c8d790" since="2025-09-01"
  if [ ! -f test262/features.txt ]; then
    git clone --single-branch --shallow-since="$since" https://github.com/tc39/test262.git || return 1
    ( cd test262 && git checkout -q "$commit" && patch -p1 < ../tests/test262.patch ) || return 1
  else
    ( cd test262 && git fetch && git reset --hard "$commit" && patch -p1 < ../tests/test262.patch ) || return 1
  fi
  echo "test2-bootstrap: ok"
}
_test2_run(){  # $@ = extra run-test262 flags
  _require_dynajs
  [ -f test262/features.txt ] || { echo "test262 tests not installed (run: ./build.sh test2-bootstrap)"; return 1; }
  engine_build $HARDEN_CFG || exit 1
  time ./run-test262 -t -m -c tools/test262.conf "$@"
}

# ============================================================================
# Fuzz machinery -- the libFuzzer link proof. The .fuzz.o mode compiles every
# object with the fuzz sanitizer set; the archives and targets below mirror
# the recipes exactly (the direct-source targets keep their hardcoded
# -fsanitize=fuzzer,address,undefined lines).
# ============================================================================
FZ_RULES="fuzz_eval fuzz_compile fuzz_regexp fuzz_regexp_compile fuzz_json \
fuzz_bytecode fuzz_bceval fuzz_module_export fuzz_net fuzz_dyns fuzz_lz4 \
fuzz_scram fuzz_codec"
FZ_NAT_RULES="fuzz_stdlib fuzz_parsers fuzz_dataframe fuzz_oauth2 fuzz_csv"

fz_setup(){  # $@ = CONFIG_* args; parses config + flags WITHOUT the stamp wipe
  eng_parse_cfg "$@" || return 1
  eng_detect
  eng_flags || return 1
  eng_objects
  mkdir -p "$OBJDIR" "$OBJDIR/examples" "$OBJDIR/tests"
  FUZZ_SAN_FLAGS=""
  FUZZ_SAN_WARN_CMD="true"
  LIB_FUZZING_ENGINE="${LIB_FUZZING_ENGINE:--fsanitize=fuzzer}"
  if [ -z "${FUZZ_NO_DEFAULT_SAN:-}" ] && [ -z "${CONFIG_ASAN:-}" ] \
     && [ -z "${CONFIG_MSAN:-}" ] && [ -z "${CONFIG_TSAN:-}" ]; then
    FUZZ_SAN_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
  elif [ -n "${FUZZ_NO_DEFAULT_SAN:-}" ] && [ -z "${CONFIG_ASAN:-}" ] && [ -z "${CONFIG_MSAN:-}" ]; then
    # a fuzz target with no sanitizer catches CRASHES only: a planted one-byte
    # overread survived 5229 executions unreported (src/fuzz/README)
    FUZZ_SAN_WARN_CMD="printf '%s' '\n  WARNING: built with NO memory sanitizer.\n    libFuzzer alone reports crashes only -- an out-of-bounds read produces\n    nothing. Drop FUZZ_NO_DEFAULT_SAN before trusting a clean run.\n\n'"
  fi
  SIMD_FUZZ_OBJS=""
  local b
  for b in $SIMD_FUZZ_BASE; do SIMD_FUZZ_OBJS="$SIMD_FUZZ_OBJS $OBJDIR/$b.fuzz.o"; done
  SIMD_FUZZ_OBJS="${SIMD_FUZZ_OBJS# }"
  ENG_POOL_LOGDIR=""
  return 0
}
SIMD_FUZZ_BASE="dyna-simd-core dyna-simd-scalar dyna-simd-neon dyna-simd-sse42 dyna-simd-avx2 dyna-simd-avx512 dyna-simd-sve"

_fz_one_obj(){  # $1 = object basename (fuzz_eval, libregexp, ...); extra=-I.
  local src obj
  if [ -f "src/fuzz/$1.c" ]; then src="src/fuzz/$1.c"
  else src=$(eng_src_of "$1") || { echo "FAIL: no source for fuzz object $1"; return 1; }
  fi
  obj="$OBJDIR/$1.fuzz.o"
  eng_stale "$obj" "$src" || return 0
  printf '%s\t%s\tfuzz\t-I.\n' "$src" "$obj"
}
_fz_archive(){  # $1 = out archive; $2.. = object basenames
  local out="$1"; shift
  local specs="" src obj b members=""
  for b in "$@"; do
    src=$(eng_src_of "$b") || { echo "FAIL: no source for $b.c"; return 1; }
    obj="$OBJDIR/$b.fuzz.o"
    members="$members $obj"
    eng_stale "$obj" "$src" && specs+="$src"$'\t'"$obj"$'\t'"fuzz"$'\t'"-I."$'\n'
  done
  eng_pool "$specs" || return 1
  # shellcheck disable=SC2086
  if eng_out_stale "$out" $members; then
    # shellcheck disable=SC2086
    "$ENG_AR" rcs "$out" $members || { echo "FAIL: AR $out"; return 1; }
    echo "  AR $out"
  fi
}
# fz_link_one: the archive-based link lines (CFLAGS_OPT + the fuzz sanitizer
# set + the libFuzzer engine). nat targets also carry the runtime libs and
# print the no-sanitizer warning when one was opted out.
fz_link_one(){
  local t="$1" objs warn=0
  case "$t" in
    fuzz_eval|fuzz_compile|fuzz_bceval)
      objs="$OBJDIR/$t.fuzz.o $OBJDIR/fuzz_common.fuzz.o libdynajs.fuzz.a" ;;
    fuzz_regexp_compile|fuzz_json|fuzz_bytecode|fuzz_module_export)
      objs="$OBJDIR/$t.fuzz.o libdynajs.fuzz.a" ;;
    fuzz_regexp)
      objs="$OBJDIR/fuzz_regexp.fuzz.o $OBJDIR/libregexp.fuzz.o $OBJDIR/cutils.fuzz.o $OBJDIR/libunicode.fuzz.o $SIMD_FUZZ_OBJS" ;;
    fuzz_dataframe|fuzz_stdlib|fuzz_parsers|fuzz_oauth2)
      objs="$OBJDIR/$t.fuzz.o $OBJDIR/fuzz_common.fuzz.o libdynajs-nat.fuzz.a"; warn=1 ;;
    *) echo "FAIL: no fuzz link recipe for $t"; return 99 ;;
  esac
  echo "  LINK $t"
  # shellcheck disable=SC2086
  $ENG_CC $ENG_CFLAGS_OPT $FUZZ_SAN_FLAGS $objs -o "$t" $LIB_FUZZING_ENGINE \
    ${warn:+$ENG_LIBS} || return 1
  [ "$warn" = 1 ] && $FUZZ_SAN_WARN_CMD
  return 0
}
# fz_direct_one: the five hand-rolled single-command targets (their exact
# flags: -O1 -g -fsanitize=fuzzer,address,undefined) plus fuzz_csv.
fz_direct_one(){
  local t="$1"
  case "$t" in
    fuzz_codec)
      # shellcheck disable=SC2086
      $ENG_CC -O1 -g -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer \
        -Isrc/core -Isrc -o fuzz_codec src/fuzz/fuzz_codec.c src/core/dyn-codec.c $SIMD_SRCS ;;
    fuzz_dyns)
      $ENG_CC -O1 -g -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer \
        -Isrc/core -o fuzz_dyns src/fuzz/fuzz_dyns.c src/core/dyn-serial.c src/core/dyn-hash.c ;;
    fuzz_lz4)
      $ENG_CC -O1 -g -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer \
        -Isrc/core -o fuzz_lz4 src/fuzz/fuzz_lz4.c src/core/dyn-compress.c src/core/dyn-hash.c ;;
    fuzz_scram)
      # shellcheck disable=SC2086
      $ENG_CC -O1 -g -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer \
        -Isrc/core -Isrc -o fuzz_scram src/fuzz/fuzz_scram.c src/core/dyn-scram.c \
        src/core/dyn-hash.c src/core/dyn-codec.c src/core/dyn-prng.c $SIMD_SRCS ;;
    fuzz_net)
      # shellcheck disable=SC2086
      $ENG_CC -O1 -g -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer \
        -Isrc/core -Isrc -o fuzz_net src/fuzz/fuzz_net.c src/core/dyn-resp.c \
        src/core/dyn-dns.c src/core/dyn-scram.c src/core/dyn-hash.c \
        src/core/dyn-codec.c src/core/dyn-prng.c src/dtoa.c src/cutils.c $SIMD_SRCS ;;
    fuzz_csv)
      [ -n "$(cfg_v NATIVE_MODULES)" ] || {
        echo "fuzz_csv needs CONFIG_NATIVE_MODULES=y: dyna-csv.c is gated on"
        echo "CONFIG_NATIVE_MODULE_CSV and compiles to nothing without it."
        return 1; }
      $ENG_CC -O1 -g -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer \
        -I. -Isrc -Isrc/core -DCONFIG_NATIVE_MODULES -DCONFIG_NATIVE_MODULE_CSV \
        -o fuzz_csv src/fuzz/fuzz_csv.c src/dyna-nat.c libdynajs.fuzz.a ;;
    *) echo "FAIL: no direct fuzz recipe for $t"; return 99 ;;
  esac
  echo "  LINK $t"
}
fz_is_direct(){ case "$1" in fuzz_codec|fuzz_dyns|fuzz_lz4|fuzz_scram|fuzz_net|fuzz_csv) return 0 ;; *) return 1 ;; esac; }

# build the named fuzz targets (default: every rule). Archive objects and
# archives first (the compile pool), then the links in parallel.
fz_build(){
  # the fuzz pool and links keep their own logs: the engine build's 0-warning
  # sweep reads .build-logs/.link-logs, and a fuzz-object warning (or a
  # linker note) must never fail an unrelated engine build
  ENG_POOL_LOGDIR="$OBJDIR/.fuzz-logs"
  mkdir -p "$ENG_POOL_LOGDIR"
  # shellcheck disable=SC2034
  local targets="$*" arch=0 natarch=0 t specs="" src obj b
  [ -n "$targets" ] || targets="$FZ_RULES $FZ_NAT_RULES"
  for t in $targets; do
    if fz_is_direct "$t"; then continue; fi
    case "$t" in
      fuzz_dataframe|fuzz_stdlib|fuzz_parsers|fuzz_oauth2) natarch=1 ;;
      fuzz_regexp) : ;;
      *) arch=1 ;;
    esac
    src=$(_fz_one_obj "$t") || return 1
    [ -n "$src" ] && specs+="$src"$'\n'
  done
  if [ "$arch" = 1 ]; then
    for b in $ENG_LIB_OBJS; do
      src=$(_fz_one_obj "$b") || return 1
      [ -n "$src" ] && specs+="$src"$'\n'
    done
  fi
  if [ "$natarch" = 1 ]; then
    for b in $ENG_LIB_OBJS $ENG_NAT_OBJS; do
      src=$(_fz_one_obj "$b") || return 1
      [ -n "$src" ] && specs+="$src"$'\n'
    done
  fi
  for b in $SIMD_FUZZ_BASE; do
    src=$(_fz_one_obj "$b") || return 1
    [ -n "$src" ] && specs+="$src"$'\n'
  done
  # the shared helper object the eval/compile/bceval links pull in
  src=$(_fz_one_obj fuzz_common) || return 1
  [ -n "$src" ] && specs+="$src"$'\n'
  eng_pool "$specs" || return 1
  [ "$arch" = 1 ] && _fz_archive libdynajs.fuzz.a $ENG_LIB_OBJS
  [ "$natarch" = 1 ] && _fz_archive libdynajs-nat.fuzz.a $ENG_LIB_OBJS $ENG_NAT_OBJS
  # the links, in parallel (eng_bg records the rc; reap reports any failure)
  local llog="$ENG_POOL_LOGDIR"
  rm -f "$llog"/fz-*.log "$llog"/fz-*.rc 2>/dev/null
  ENG_BG_RCS=()
  for t in $targets; do
    echo "  LINK $t"
    if fz_is_direct "$t"; then
      eng_bg "$llog/fz-$t.log" fz_direct_one "$t"
    else
      eng_bg "$llog/fz-$t.log" fz_link_one "$t"
    fi
  done
  if eng_bg_reap; then
    # green run: drop the logs (warnings in a fuzz TU stay visible in the
    # console output above; a failed run keeps everything for the post-mortem)
    rm -rf "$ENG_POOL_LOGDIR"
    ENG_POOL_LOGDIR=""
    return 0
  fi
  return 1
}

fz_audit_run(){
  local n src orphan="" unbuild=0 t
  n=$(printf '%s\n' $FZ_RULES $FZ_NAT_RULES | sort -u | wc -l | tr -d ' ')
  for src in $(ls src/fuzz/fuzz_*.c 2>/dev/null | sed 's|src/fuzz/||; s|\.c$||'); do
    [ "$src" = fuzz_common ] && continue
    case " $FZ_RULES $FZ_NAT_RULES " in *" $src "*) ;; *) orphan="$orphan $src" ;; esac
  done
  if [ -n "$orphan" ]; then
    echo "FAIL: fuzz sources with no build rule (never compiled, never run):"
    echo "$orphan"
    return 1
  fi
  for t in $FZ_RULES $FZ_NAT_RULES; do
    if fz_is_direct "$t"; then continue; fi
    case "$t" in
      fuzz_eval|fuzz_compile|fuzz_bceval|fuzz_regexp_compile|fuzz_json|fuzz_bytecode|fuzz_module_export|fuzz_regexp|fuzz_dataframe|fuzz_stdlib|fuzz_parsers|fuzz_oauth2) ;;
      *) echo "FAIL: gated but unbuildable -- in the fuzz tables with no recipe: $t"; unbuild=1 ;;
    esac
  done
  [ "$unbuild" = 0 ] || return 1
  echo "fuzz-audit: all $n fuzz targets gated"
}
fz_tls_link_audit(){
  local bad
  bad=$(grep -E '^#include "' src/dyna-tls.c 2>/dev/null | grep -v 'dyna-tls.h' || true)
  if [ -n "$bad" ]; then
    echo "FAIL: dyna-tls.c gained an engine include, so the fuzz/net hand-written"
    echo "      source lists are now short:"
    echo "$bad"
    return 1
  fi
  echo "tls-link-audit: dyna-tls.c is still engine-free"
}
fz_all_run(){
  fz_setup CONFIG_NATIVE_MODULES=y || return 1
  fz_audit_run || return 1
  fz_build || return 1
  local bad=0 t a u
  for t in $FZ_RULES $FZ_NAT_RULES; do
    if [ ! -x "./$t" ]; then echo "MISSING: $t was not built"; bad=1; continue; fi
    a=$(nm "./$t" 2>/dev/null | grep -c __asan_init)
    u=$(nm "./$t" 2>/dev/null | grep -c __ubsan_handle)
    if [ "${a:-0}" -lt 1 ]; then echo "FAIL: $t has NO AddressSanitizer"; bad=1
    elif [ "${u:-0}" -lt 1 ]; then
      echo "FAIL: $t has NO UndefinedBehaviorSanitizer (built with CONFIG_ASAN=y?)"; bad=1
    else echo "  ok  $t  asan=$a ubsan=$u"; fi
  done
  [ "$bad" -eq 0 ] || return 1
  echo "fuzz-all: every target carries address+undefined"
}
fz_smoke_run(){
  local runs="${FUZZ_SMOKE_RUNS:-4000}" runs_df="${FUZZ_SMOKE_RUNS_DATAFRAME:-500}"
  # artifacts land in this process's private in-tree scratch dir, never the
  # fixed world-writable /tmp/dyna-fuzzart another user could pre-create
  local art="$DEV_SCRATCH/fuzzart"
  mkdir -p "$art"
  local rc=0 t corp n
  for t in $FZ_RULES $FZ_NAT_RULES; do
    if [ ! -x "./$t" ]; then echo "  SKIP $t (not built -- run ./build.sh fuzz-all)"; continue; fi
    # runtime corpus state lives in the artifacts tree, never in src/;
    # tracked seeds under src/fuzz/corpus_<name> are copied in read-only.
    corp="$OBJDIR/fuzz-corpus/$t"
    seeds=""
    case "$t" in
      fuzz_stdlib)    seeds=src/fuzz/corpus_robots ;;
      fuzz_parsers)   seeds=src/fuzz/corpus_robots ;;
      fuzz_dataframe) seeds=src/fuzz/corpus_dataframe ;;
      fuzz_bceval)    seeds=src/fuzz/corpus_bceval ;;
      fuzz_dyns)      seeds=src/fuzz/corpus_ml ;;
      fuzz_eval)      seeds=src/fuzz/corpus_eval ;;
    esac
    mkdir -p "$corp"
    [ -n "$seeds" ] && [ -d "$seeds" ] && cp -f "$seeds"/* "$corp"/ 2>/dev/null
    printf '  %-22s ' "$t"
    n=$runs
    [ "$t" = fuzz_dataframe ] && n=$runs_df
    ( cd "$art" && \
      "$ROOT/$t" ${corp:+"$ROOT/$corp"} \
        -runs="$n" -len_control=0 -max_len=8192 \
        -artifact_prefix="$art/" ) > "$art/$t.log" 2>&1
    if [ $? -eq 0 ]; then
      echo "ok  $(grep -o 'cov: [0-9]*' "$art/$t.log" | tail -1)${corp:+  seeded}"
    else
      echo "FAIL -- see $art/$t.log"; rc=1
    fi
  done
  [ "$rc" -eq 0 ] && echo "fuzz-smoke: all targets clean at $runs runs"
  return "$rc"
}
fz_prepush_run(){
  fz_setup CONFIG_NATIVE_MODULES=y || return 1
  fz_audit_run || { echo "FAIL: fuzz-audit"; return 1; }
  echo "fuzz: linking all fuzz targets (log: $OBJDIR/fuzzlink.log)..."
  if ! fz_build > "$OBJDIR/fuzzlink.log" 2>&1; then
    echo "FAIL: a fuzz target no longer links -- a hand-written recipe lost an object."
    tail -25 "$OBJDIR/fuzzlink.log"
    return 1
  fi
  echo "fuzz: all targets linked -- ok"
}

# ============================================================================
# Per-module slim binaries + module-scoped test runners (the slim/one rules).
#
# The engine is the expensive part and it is shared: every slim binary REUSES
# the main build's objects and links one extra tiny translation unit -- a
# dyna-nat.o variant compiled with only that family's registration defines.
#   mod <list>      link slim-<m> for each module, then run its mt_modtests
#                   list against it (serial across modules on purpose: the
#                   suites bind ports)
#   one <module>    link the MINIMAL binary: exactly MODDEFS_<m> (+ FILE iff
#                   dyna-file.o is linked) and the proven MINCOMPANION_<m>
#                   set (or the conservative SLIMCOMPANION fallback)
# ============================================================================
SLIM_DIR=.obj/slim

mt_mincomp(){  # the proven companion table (tools/slim-prune.sh output)
  case "$1" in
    dataframe) printf '%s' 'dyna-net.o dyna-http.o dyna-netip.o dyna-net-tcp.o dyna-net-proxy.o dyna-net-dns.o dyna-net-redis.o dyna-net-pg.o dyna-net-ratelimit.o dyna-net-metrics.o dyna-net-sqlite.o dyna-tls.o' ;;
    html|structures|time|url|vserialize)
      printf '%s' 'dyna-http.o dyna-netip.o dyna-net-tcp.o dyna-net-proxy.o dyna-net-dns.o dyna-net-redis.o dyna-net-pg.o dyna-net-ratelimit.o dyna-net-metrics.o dyna-net-sqlite.o dyna-tls.o' ;;
    *) return 1 ;;
  esac
}
_slim_companions(){
  # dyna:file rides along in every slim binary except file itself -- nearly
  # every test reads fixtures through it -- and it drags the net family it
  # needs (the shared reactor).
  local c="$OBJDIR/dyna-file.o $OBJDIR/dyna-net.o $OBJDIR/dyna-http.o \
$OBJDIR/dyna-netip.o $OBJDIR/dyna-net-tcp.o $OBJDIR/dyna-net-proxy.o \
$OBJDIR/dyna-net-dns.o $OBJDIR/dyna-net-redis.o $OBJDIR/dyna-net-pg.o \
$OBJDIR/dyna-net-ratelimit.o $OBJDIR/dyna-net-metrics.o"
  [ -n "${ENG_SQLITE_LIBS:-}" ] && c="$c $OBJDIR/dyna-net-sqlite.o"
  if [ -f "$OBJDIR/dyna-tls.o" ] || [ -n "$(cfg_v TLS)" ]; then c="$c $OBJDIR/dyna-tls.o"; fi
  printf '%s' "$(printf '%s' "$c" | tr -s ' \n' '  ' | sed 's/^ //; s/ $//')"
}
_slim_libs(){  # OpenSSL when a stale dyna-tls.o rides along without CONFIG_TLS
  local c t
  c=$(_slim_companions)
  for t in $c; do
    [ "${t##*/}" = dyna-tls.o ] || continue
    if [ -z "$(cfg_v TLS)" ]; then
      local pc
      pc="$(brew --prefix openssl@3 2>/dev/null || brew --prefix openssl 2>/dev/null)/lib/pkgconfig"
      PKG_CONFIG_PATH="$pc:${PKG_CONFIG_PATH:-}" pkg-config --libs 'openssl >= 3.0' 2>/dev/null
    fi
    return 0
  done
  return 0
}
_companions_have_file(){  # dyna-file.o linked? (token-wise: entries are objdir-prefixed)
  local t
  for t in $1; do [ "${t##*/}" = dyna-file.o ] && return 0; done
  return 1
}
_slim_nat_compile(){  # $1 = out; $2.. = registration family names
  local out="$1"; shift
  local defs="" d cf="" t
  for d in "$@"; do defs="$defs -DCONFIG_NATIVE_MODULE_$d"; done
  for t in $ENG_CFLAGS; do
    case "$t" in -DCONFIG_NATIVE_MODULE_*) ;; *) cf="$cf $t" ;; esac
  done
  mkdir -p "$(dirname "$out")"
  # shellcheck disable=SC2086
  $ENG_CC ${cf# } $defs -MMD -MF "$out.d" -c src/dyna-nat.c -o "$out" || return 1
}
_slim_link_bin(){  # $1 = out; $2.. = objects
  local out="$1"; shift
  # shellcheck disable=SC2086
  $ENG_CC $ENG_LDFLAGS $ENG_LDEXPORT -o "$out" "$@" $ENG_LIBS ${SLIM_LIBS:-}
}
# _slim_ensure KIND MODULE [COMPANION-OVERRIDE] -> SLIM_BIN
# KIND=slim: SLIMCOMPANION + MODDEFS_m + FILE. KIND=one: the MINCOMPANION_m
# set (or the override, or the SLIMCOMPANION fallback) + MODDEFS_m (+ FILE iff
# dyna-file.o is linked). The special shapes (file stdlib-os uring
# structures3) keep their hand-built forms.
_slim_ensure(){
  local kind="$1" m="$2" override="${3:-}" defs="" companions="" modobjs="" o
  local nat bin
  if [ "$kind" = one ]; then
    nat="$SLIM_DIR/one-$m/dyna-nat.o"; bin="$SLIM_DIR/dynajs-one-$m$ENG_EXE"
  else
    nat="$SLIM_DIR/$m/dyna-nat.o"; bin="$SLIM_DIR/dynajs-slim-$m$ENG_EXE"
  fi
  case "$m" in
    file|stdlib-os)
      # FILE is defined because dyna-file.o is a linked companion: without it
      # the #ifndef CONFIG_NATIVE_MODULE_FILE fallback stubs would duplicate
      # its symbols. The module under test IS the companion -- nothing smaller.
      defs="FILE" ;;
    uring)
      if [ -z "$(cfg_v IO_URING)" ] || [ ! -f src/dyna-uring.c ]; then
        echo "SKIP (not built here): uring needs CONFIG_IO_URING=y on Linux"; return 2
      fi
      defs="IO_URING FILE"; modobjs="$OBJDIR/dyna-uring.o" ;;
    structures3)
      if [ ! -f src/dyna-structures3.c ]; then
        echo "SKIP (not built here): src/dyna-structures3.c absent -- dyna:structures3 is not in this build"
        return 2
      fi
      defs="STRUCTURES3 FILE"; modobjs="$OBJDIR/dyna-structures3.o" ;;
    *)
      mt_moddefs "$m" >/dev/null || die "unknown module: $m (known: $MT_KNOWN_MODS)"
      defs="$(mt_moddefs "$m")"
      modobjs=$(mt_modobj_out "$m")
      if [ "$kind" = one ]; then
        if [ -n "$override" ]; then
          companions="$override"
        elif mt_mincomp "$m" >/dev/null 2>&1; then
          local mc
          mc=$(mt_mincomp "$m")
          companions=""
          for o in $mc; do companions="$companions $OBJDIR/$o"; done
          companions="${companions# }"
        else
          companions=$(_slim_companions)
        fi
        _companions_have_file "$companions" && defs="$defs FILE"
      else
        companions=$(_slim_companions)
        defs="$defs FILE"
      fi ;;
  esac
  SLIM_LIBS=$(_slim_libs)
  local objs=""
  for o in dyna-cli repl $ENG_LIB_OBJS dyna-aio dyna-aio-uring dyna-evloop; do
    objs="$objs $OBJDIR/$o.o"
  done
  objs="$objs $companions ${modobjs:+ $modobjs}"
  if eng_out_stale "$nat" src/dyna-nat.c; then
    echo "  CC ${nat#$SLIM_DIR/} (registration:$defs)"
    # shellcheck disable=SC2086
    _slim_nat_compile "$nat" $defs || { echo "FAIL: slim dyna-nat.o compile"; return 1; }
  fi
  objs="$objs $nat"
  # shellcheck disable=SC2086
  if eng_out_stale "$bin" $objs; then
    echo "  LINK $bin"
    # shellcheck disable=SC2086
    _slim_link_bin "$bin" $objs || { echo "FAIL: slim link $bin"; return 1; }
  fi
  SLIM_BIN="$bin"
}
# run a module's mt_modtests list against its slim binary
_slim_runtests(){
  local m="$1" bin="$2" tests
  tests=$(mt_modtests "$m" 2>/dev/null) || tests=""
  if [ -z "$tests" ]; then
    echo "test-mod-$m: NO JS TESTS LISTED (the module's mt_modtests list is empty)"
    return 0
  fi
  # shellcheck disable=SC2086
  DEV_JOBS=${DEV_JOBS:-$NCPU} DYNAJS="./$bin" ./tools/run-tests-parallel.sh $tests
}
_mod_run(){  # $@ = CONFIG_* args + module selection (args, MODS=a,b, else all)
  local cfg="" sel="" a m rc=0
  for a in "$@"; do
    case "$a" in
      CONFIG_*) cfg="$cfg $a" ;;
      MODS=*) sel="${a#MODS=}" ;;
      *) sel="$sel $a" ;;
    esac
  done
  [ -n "$sel" ] || sel="${MODS:-}"
  [ -n "$sel" ] || sel="$MT_SLIM_MODS"
  # shellcheck disable=SC2086
  engine_build ${cfg:-$NATIVE_CFG} || exit 1
  eng_objects
  mkdir -p "$SLIM_DIR"
  for m in $(printf '%s' "$sel" | tr ',' ' '); do
    case " $MT_SLIM_MODS " in
      *" $m "*) ;;
      *) die "unknown module(s) in MODS: $m. Known: $MT_SLIM_MODS" ;;
    esac
    echo "mod: $m"
    SLIM_BIN=""
    _slim_ensure slim "$m"; skip_rc=$?
    if [ "$skip_rc" = 2 ]; then
      echo "mod: $m SKIP (not built in this configuration)"
    elif [ "$skip_rc" = 0 ]; then
      _slim_runtests "$m" "$SLIM_BIN" || rc=1
    else
      rc=1
    fi
  done
  [ "$rc" = 0 ] && echo "mod: ok"
  return "$rc"
}
_one_run(){  # $@ = MODULE [--build-only] [--companions="obj list"] [CONFIG_*...]
  local m="" buildonly="" comps="" cfg="" a
  for a in "$@"; do
    case "$a" in
      --build-only) buildonly=1 ;;
      --companions=*) comps="${a#--companions=}" ;;
      CONFIG_*) cfg="$cfg $a" ;;
      *) [ -n "$m" ] && die "one: exactly one module per run (got: $m and $a)"; m="$a" ;;
    esac
  done
  [ -n "$m" ] || die "usage: one MODULE [--build-only] [--companions=\"obj list\"]"
  # shellcheck disable=SC2086
  engine_build ${cfg:-$NATIVE_CFG} || exit 1
  eng_objects
  mkdir -p "$SLIM_DIR"
  SLIM_BIN=""
  case "$m" in
    # hand-shaped modules reuse their test-mod-* binaries as their one shape
    file|stdlib-os|uring|structures3) _slim_ensure slim "$m" || exit $? ;;
    *) _slim_ensure one "$m" "$comps" || exit $? ;;
  esac
  if [ -n "$buildonly" ]; then
    echo "one $m: $SLIM_BIN"
    return 0
  fi
  _slim_runtests "$m" "$SLIM_BIN"
}
_slim_vars_run(){  # $@ = CONFIG_* args, MOD=<module>; the variable dump the
                   # pruner (tools/slim-prune.sh) parses
  local mod="" a
  for a in "$@"; do
    case "$a" in MOD=*) mod="${a#MOD=}" ;; CONFIG_*) : ;; *) die "slim-vars: bad arg $a" ;; esac
  done
  eng_parse_cfg >/dev/null 2>&1 || true
  eng_detect
  eng_flags || return 1
  eng_objects
  local slib; slib=$(_slim_libs)
  echo "SLIM_MODS=$MT_SLIM_MODS"
  printf 'ENGINE_OBJS='
  local o
  for o in dyna-cli repl $ENG_LIB_OBJS dyna-aio dyna-aio-uring dyna-evloop; do printf '%s ' "$OBJDIR/$o.o"; done
  echo ""
  echo "SLIMCOMPANION=$(_slim_companions)"
  if [ -n "$mod" ]; then
    echo "MODOBJ=$(mt_modobj_out "$mod" 2>/dev/null || true)"
    echo "MODDEFS=$(mt_moddefs "$mod" 2>/dev/null || true)"
    echo "MODTESTS=$(mt_modtests "$mod" 2>/dev/null || true)"
  else
    echo "MODOBJ="
    echo "MODDEFS="
    echo "MODTESTS="
  fi
  echo "MULTI_TESTS=$MT_MULTI"
  echo "CFLAGS=$ENG_CFLAGS"
  echo "LIBS=$ENG_LIBS $slib"
  echo "OPENLIBM=third_party/openlibm/libopenlibm.a"
  echo "EXE=$ENG_EXE"
}

# ============================================================================
# THE PROOF GATE -- absorbed verbatim-in-spirit from tools/prepush-parallel.sh
# (deleted), so build.sh is the single source of truth for building AND gating.
#
# Scheduled by DEPENDENCY instead of habit, in two waves (the wave structure and
# every rule below were measured into existence in the ws-build session):
#   wave A   codegraph + defects + import/orphan audit (parse-only, pure readers
#            of src/tests) run BESIDE the clean+build (which owns .obj). A stage
#            that parses sources cannot race a stage that writes objects, so
#            the lints ride in the build's shadow.
#   serial   the test262 leg: the FULL corpus, every gate (see the loud note in
#            gate_run), then smoke TEST.js files when any were named.
#   wave B   after the build: the suites on a PREPUSH_PAR worker pool (default
#            3), with two scheduling rules:
#            - the fuzz link proof runs FIRST: it is the only pool candidate
#              that writes .obj at scale (libdynajs.fuzz.a, fuzz_* binaries),
#              and concurrent with the core suite's bjson compile it died with
#              "Bad file descriptor" under fd pressure;
#            - test-native holds the full core budget (it is the critical path:
#              ~240s of 403 suites) while api/security/repl/tls/blackbox are
#              single-digit-minute stragglers; coretest and security keep the
#              NCPU/PAR fan cap so concurrent suites cannot pile up.
#            Contention was checked, not assumed: the pure-run stages share no
#            test file and no /tmp literal (scanned: core x api x security = 0
#            shared files, 0 tmp overlap), so their fixtures cannot collide.
#
# Fail-closed: any non-zero stage (or a red lint in wave A) fails the gate;
# every pool stage is bounded by PREPUSH_TIMEOUT and logs to .prepush/<stage>.log
# (NOT .obj/: a clean build wipes it mid-gate); the duration lands in
# .prepush/<stage>.ok, rc in .prepush/<stage>.rc. A failed leg prints its first
# 5 error/assert/FAIL lines, how long it ran, the exact command to reproduce it,
# and a re-run hint (flakes run ~1 in 70 -- a green re-run was one).
#
# Scope: bare -> derived (tools/affected-modules.sh, against @{push}..HEAD when
# @{push} resolves, else HEAD); `core`/`infra` in the scope or an empty
# derivation mean FULL -- and the stages say so; MODULES=a,b (or positional
# names) scope the native+blackbox legs; TEST_SCOPE=all / --full is the
# deliberate full-matrix escape. PREPUSH_MODULES overrides the derivation. The
# ambient MODULES/TEST_SCOPE environment is SCRUBBED at the gate door: the gate
# decides its own scope, and a parent's exported knob (this runs from git
# hooks) must never silently filter a stage the gate decided to run full.
#
# Knobs: PREPUSH_J (build -j), PREPUSH_PAR (wave-B concurrency, default 3),
# PREPUSH_TIMEOUT (per-stage seconds, default 1800), PREPUSH_DEBUG=1 (echo
# every wrapped command + rc + elapsed as "[prepush]" lines), PREPUSH_MODULES,
# PREPUSH_MULTI_TESTS (the full native matrix's MULTI_TESTS override).
# One leg only (debug a failure without the full gate):
#        ./build.sh __gate-stage <name>   (e.g. repl, coretest, tls)
#        -- suite legs need the tree the gate's build already produced;
#           unknown names exit 97.
# ============================================================================

GATE_LOGDIR=.prepush
GATE_T262_TESTS=83744
g_say(){ echo "$*"; }

gate_scope_modules(){
  # The scope the native+blackbox legs act on: PREPUSH_MODULES wins (the gate
  # itself exports it, so __gate-stage children re-derive nothing); unset, the
  # same derivation test-native and `test` use.
  if [ -n "${PREPUSH_MODULES:-}" ]; then printf '%s\n' "$PREPUSH_MODULES"; return 0; fi
  if git rev-parse --verify --quiet '@{push}' >/dev/null 2>&1; then
    ./tools/affected-modules.sh '@{push}..HEAD' 2>/dev/null
  else
    ./tools/affected-modules.sh HEAD 2>/dev/null
  fi
}

g_run(){
  if [ "${PREPUSH_DEBUG:-0}" != 1 ]; then "$@"; return $?; fi
  local t0 rc
  t0=$(_now)
  echo "[prepush] \$ $*" >&2
  "$@"; rc=$?
  echo "[prepush] rc=$rc $(_elapsed "$t0" "$(_now)")s: $*" >&2
  return "$rc"
}

g_fan_cap(){ local c=$(( NCPU / GATE_PAR )); [ "$c" -lt 1 ] && c=1; echo "$c"; }

# --- the stages (each also reachable via ./build.sh __gate-stage <name>) ------

g_stage_codegraph(){
  g_run python3 bench/codegraph.py . --report > bench/codegraph_report.txt 2>&1 || return 1
  local sum nf nfn
  sum=$(grep -E '^codegraph-[a-z0-9]+: [0-9]+ files parsed' bench/codegraph_report.txt | head -1)
  nf=$(printf '%s' "$sum" | sed -E 's/^codegraph-[a-z0-9]+: ([0-9]+) files.*/\1/')
  nfn=$(grep -E '^ *[0-9]+ symbols parsed' bench/codegraph_report.txt | head -1 | sed -E 's/ *([0-9]+) symbols.*/\1/')
  case "$nf$nfn" in ''|*[!0-9]*) nf=0; nfn=0;; esac
  [ "$nf" -ge 1 ] && [ "$nfn" -ge 1 ] || { echo "codegraph parsed $nf files / $nfn symbols -- wrong root or moved tree."; return 1; }
  echo "$sum ($nfn symbols)"
}
g_stage_defects(){
  # the selftest plants one defect per class and proves the scanner fires on
  # it: a scanner that cannot fail proves nothing.
  g_run python3 tools/test_codegraph_defects.py >/dev/null 2>&1 \
    || { echo "defect selftest FAILED (a check lost its plant)"; return 1; }
  g_run python3 bench/codegraph.py . --defects >/dev/null 2>&1 \
    || { echo "defect gate: ERROR-severity finding present; run: python3 bench/codegraph.py . --defects --defects-all"; return 1; }
  echo "defect selftest + baseline gate clean"
}
g_stage_imports(){
  g_run python3 tools/check-orphan-tests.py || return 1
  echo "orphan test audit clean"
}
g_stage_build(){
  g_run ./build.sh clean >/dev/null || return 1
  DEV_BUILD_J="$GATE_J" g_run ./build.sh build $NATIVE_CFG || return 1
  ./dynajs -e 'import("dyna:mathx")' >/dev/null 2>&1 || {
    echo "gate built a binary that cannot load dyna:*. Suites would SKIP and print green."; return 1; }
  echo "build ok (-j$GATE_J, $NATIVE_CFG)"
}
g_stage_fuzz(){
  # the link proof logs to $OBJDIR/fuzzlink.log, NOT the stage log: the compile
  # pool's warning sweep and the gate's log stay separate (fuzz log isolation)
  DEV_BUILD_J="$GATE_J" g_run ./build.sh prepush-fuzz
}
g_stage_coretest(){
  DEV_JOBS=$(g_fan_cap) g_run ./build.sh test-core TEST_SCOPE=all
}
g_stage_tls(){
  # the probe-first runners (never rebuild); the remaining legs are x509+aead.
  # BOTH legs are load-bearing: test_x509.js is in MT_STANDALONE_TABLE (so the
  # native stage skips it) and this stage used to report only the LAST
  # command's status, which made a failing x509 run invisible to the gate.
  g_run ./build.sh test-x509 || { echo "tls: test-x509 failed (diagnostics above)"; return 1; }
  g_run ./build.sh test-crypto-aead || { echo "tls: test-crypto-aead failed (diagnostics above)"; return 1; }
  echo "tls: x509 + crypto-aead ok"
}
g_stage_native(){
  if [ "${GATE_SCOPE_ALL:-0}" = 1 ]; then
    g_say "native: TEST_SCOPE=all -- full matrix"
    g_full_native; return $?
  fi
  local mods
  mods=$(gate_scope_modules)
  case " $mods " in
    *" core "*)  g_say "native: core touched -- full matrix (scope:$mods)"; g_full_native; return $? ;;
    *" infra "*) g_say "native: infra touched -- full matrix (scope:$mods)"; g_full_native; return $? ;;
  esac
  if [ -z "$mods" ]; then
    g_say "native: full matrix (no modules derived -- scope one deliberately: PREPUSH_MODULES=csv,net)"
    g_full_native; return $?
  fi
  g_say "native: scoped to [$mods] (the deliberate full matrix is: ./build.sh gate TEST_SCOPE=all)"
  DEV_JOBS=$NCPU g_run ./build.sh test-native "MODULES=$mods"
}
g_full_native(){
  if [ -n "${PREPUSH_MULTI_TESTS:-}" ]; then
    DEV_JOBS=$NCPU g_run ./build.sh test-native \
      "MULTI_TESTS=$PREPUSH_MULTI_TESTS" TEST_SCOPE=all
  else
    DEV_JOBS=$NCPU g_run ./build.sh test-native TEST_SCOPE=all
  fi
}
g_stage_api(){
  g_run ./build.sh test-api
}
# The pen-test suites bound each probe by WALL CLOCK (default 2000 ms, to catch
# hangs and quadratics). Inside the gate three legs share the machine, so the
# same probe legitimately takes up to three times its solo time (measured: the
# slowest probe is ~0.7 s solo and crossed 2 s here on the untouched baseline
# binary too). Scale the budget by the wave's worker count; an explicit
# PEN_BUDGET_MS still wins.
g_stage_security(){
  PEN_BUDGET_MS="${PEN_BUDGET_MS:-$(( ${GATE_PAR:-3} * 2000 ))}" DEV_JOBS=$(g_fan_cap) g_run ./build.sh test-security
}
g_stage_blackbox(){
  ./dynajs -e 'import("dyna:bytes")' >/dev/null 2>&1 || {
    echo "blackbox: ./dynajs cannot load dyna:* modules (a non-native build relinked it); run ./build.sh build $NATIVE_CFG first"; return 1; }
  local mods m rc=0
  if [ "${GATE_SCOPE_ALL:-0}" = 1 ]; then
    g_say "blackbox: TEST_SCOPE=all -- full matrix"
    g_run sh tests/blackbox/run.sh ./dynajs; return $?
  fi
  mods=$(gate_scope_modules)
  case " $mods " in
    *" core "*)  g_say "blackbox: core touched -- full matrix (scope:$mods)"; g_run sh tests/blackbox/run.sh ./dynajs; return $? ;;
    *" infra "*) g_say "blackbox: infra touched -- full matrix (scope:$mods)"; g_run sh tests/blackbox/run.sh ./dynajs; return $? ;;
  esac
  if [ -z "$mods" ]; then
    g_say "blackbox: full matrix (no modules derived)"
    g_run sh tests/blackbox/run.sh ./dynajs; return $?
  fi
  g_say "blackbox: scoped to [$mods] (bb_<m> substring filters -- run.sh takes one per call)"
  for m in $mods; do
    g_run sh tests/blackbox/run.sh ./dynajs "bb_$m" || rc=1
  done
  return $rc
}
g_stage_repl(){
  g_run ./build.sh test-repl
}
g_stage_t262(){
  # isolated build: the runner needs no dyna:* modules and must not clobber
  # the developer's native ./dynajs. Its own OUT_BASE means its own objdir AND
  # its own lock (B1-14), so this leg never shares a build tree with another.
  OUT="$ROOT/.obj-t262" g_run ./build.sh build $HARDEN_CFG || {
    echo "t262: the runner build failed (diagnostics above)"; return 1; }
  _t262_full "$ROOT/.obj-t262/run-test262"
}

# --- the C regression stage (B1-08) -----------------------------------------
# The only ASan/UBSan/TSan-instrumented runs in the project lived in
# _c_regression_targets, reachable ONLY from the command dispatch: no gate
# stage called any of them. So a memory-lifetime regression in a C module
# passed the gate. This stage runs them, each externally bounded.
#
# What is deliberately NOT here (each reported, never silently skipped):
#  - the three docker io_uring targets (need a Linux container + the dynajs:deps
#    image; they are the io_uring backend, not a regression target of this tree)
#  - bench-core-tus / bench-ac (benchmarks, not verdicts)
#  - test-bc-read-safety / test-stack-limit when third_party/openlibm is absent:
#    they hard-name that archive, so a clean clone cannot link them. They are
#    reported by name as UNLINKED-HERE rather than counted as coverage.
CTEST_BENCH="bench-core-tus bench-ac"
CTEST_DOCKER="test-uring-sendfile test-uring-connect-offload test-uring-tick"
CTEST_OPENLIBM="test-bc-read-safety test-stack-limit"
CTEST_LIST="test-io-atomic test-io-slurp-cap test-regexp-prefilter test-dtoa-subnormal \
test-simd-bitmap test-ds-core test-ac-caps test-ds-btree-oom test-pool test-dns-codec \
test-resp-codec test-scram test-timer test-aio-sendfile-busy test-aio-connect-offload \
test-dns-foreign-answers test-aio-disk test-exec-spawn test-crc32c-hw test-crc32c-race \
test-sha256-hw test-simd-f64 test-simd-int test-simd-reductions test-text-kernels \
test-utf16-kernels test-simd-tiers test-simd-x86 test-socket-cloexec test-evloop-cloexec"
CTEST_LINKED="test-aio-tls test-net-final-sweep test-net-udp-grave"
# Targets known to FAIL on today's engine, with the defect each one pins. This
# is an UN-ROT PIN, not a mute: if a pinned target starts passing the stage
# FAILS with XPASS, so the pin cannot outlive its bug. Wiring these targets
# into a gate is what surfaced them in the first place (B1-08: no C regression
# target ran in any stage, so none of them was known to be red).
#   test-dns-codec  dyn_dns_name_decode() rejects a compression pointer that
#                   targets the MIDDLE of a name ("ns" + pointer to the
#                   "example" inside "www.example.com"): src/core/dyn-dns.c
#                   guards the loop with `target >= limit` where limit starts
#                   at the caller's offset, so target(4) >= off(17) returns
#                   DYN_DNS_E_LOOP. Legal, common wire format -> the DNS client
#                   cannot resolve such a name. TICKET: B1-08/dns-name-midptr.
CTEST_XFAIL="test-dns-codec"
g_stage_ctest(){
  local t rc=0 ran=0 skipped=0 xfail_bad=0 logdir mo="${CTEST_TIMEOUT:-600}"
  logdir="$DEV_STATE/ctest-logs"; mkdir -p "$logdir"
  echo "ctest: $(printf '%s\n' $CTEST_LIST $CTEST_LINKED | grep -v '^$' | wc -l | tr -d ' ') C regression targets, each ASan/UBSan-instrumented and externally bounded"
  # the targets that link the CONFIGURED engine need its objects to exist
  eng_parse_cfg CONFIG_NATIVE_MODULES=y >/dev/null 2>&1 || true
  eng_detect; eng_flags || { echo "ctest: eng_flags failed"; return 1; }; eng_objects
  for t in $CTEST_LIST; do
    ran=$((ran+1))
    # shellcheck disable=SC2086
    BOUNDED_RUN_MARKER="$logdir/$t.bounded" \
      ./tools/bounded-run.sh "$mo" "$(( mo > 300 ? 300 : mo ))" "ctest:$t" -- \
      ./build.sh "$t" >"$logdir/$t.log" 2>&1
    rc=$?
    if [ "$rc" = 0 ]; then
      case " $CTEST_XFAIL " in
        *" $t "*) echo "  ctest XPASS $t -- it is pinned as a KNOWN FAILURE but passes now;"
                   echo "             remove it from CTEST_XFAIL in build.sh."
                   xfail_bad=1 ;;
        *)        printf '  ctest ok   %-28s\n' "$t" ;;
      esac
    elif [ -f "$logdir/$t.bounded" ]; then
      echo "  ctest TIMEOUT $t (external ${mo}s bound)"; rc=1
    else
      case " $CTEST_XFAIL " in
        *" $t "*)
          xfail_bad=0
          echo "  ctest XFAIL $t -- pinned, known-red on today's engine (see CTEST_XFAIL in build.sh):"
          grep -m3 -E "FAIL:" "$logdir/$t.log" | sed 's/^/      /'
          skipped=$((skipped+1)) ;;
        *)
          echo "  ctest FAIL  $t (rc=$rc)"; tail -12 "$logdir/$t.log" | sed 's/^/      /'; rc=1 ;;
      esac
    fi
  done
  for t in $CTEST_LINKED; do
    ran=$((ran+1))
    BOUNDED_RUN_MARKER="$logdir/$t.bounded" \
      ./tools/bounded-run.sh "$mo" "$(( mo > 300 ? 300 : mo ))" "ctest:$t" -- \
      ./build.sh "$t" >"$logdir/$t.log" 2>&1
    rc=$?
    [ "$rc" = 0 ] && printf '  ctest ok   %-28s\n' "$t" \
      || { echo "  ctest FAIL  $t (rc=$rc)"; tail -12 "$logdir/$t.log" | sed 's/^/      /'; rc=1; }
  done
  # explicit, NAMED skips -- the coverage claim is only as good as these lines
  for t in $CTEST_OPENLIBM; do
    if [ -f third_party/openlibm/libopenlibm.a ]; then
      ran=$((ran+1))
      BOUNDED_RUN_MARKER="$logdir/$t.bounded" \
        ./tools/bounded-run.sh "$mo" "$(( mo > 300 ? 300 : mo ))" "ctest:$t" -- \
        ./build.sh "$t" >"$logdir/$t.log" 2>&1
      rc=$?
      [ "$rc" = 0 ] && printf '  ctest ok   %-28s\n' "$t" \
        || { echo "  ctest FAIL  $t (rc=$rc)"; tail -12 "$logdir/$t.log" | sed 's/^/      /'; rc=1; }
    else
      skipped=$((skipped+1))
      echo "  ctest SKIP  $t (needs third_party/openlibm/libopenlibm.a -- git-ignored;"
      echo "             build it once and this row runs. NOT counted as coverage.)"
    fi
  done
  for t in $CTEST_DOCKER; do
    skipped=$((skipped+1)); printf '  ctest SKIP  %-28s (docker io_uring backend; ./build.sh %s)\n' "$t" "$t"
  done
  [ "$xfail_bad" != 0 ] && rc=1
  echo "ctest: $ran targets run, $skipped explicitly skipped (incl. pinned known-failures), logs in $logdir"
  return "$rc"
}

# --- the standalone-suite stage (B1-08) -------------------------------------
# native_tests() subtracts MT_STANDALONE_TABLE from the native list, and no
# other stage re-ran most of those entries: the HTTP server suite, the HTTP
# async suite, the WebSocket suite and the TLS server suite were in NO stage
# at all -- exactly the four that would catch a regression in the server stack.
# tests/test_tls_session.js had no table entry either.
#
# The list is DERIVED (standalone minus everything another stage owns), so
# removing a stage's ownership removes the row instead of double-running the
# suite -- and adding a standalone suite puts it here automatically.
STANDALONE_EXTRA="tests/test_tls_session.js tests/test_scrape_autoclient.js"
# Suites that are in the standalone table but CANNOT run on this host. Named,
# printed, and never counted as coverage -- the module they need is a Linux
# io_uring backend and the engine reports "could not load module filename
# dyna:uring" on macOS. Its real run is docker: ./build.sh test-uring.
STANDALONE_HOST_SKIP="tests/test_uring_disk.js"
standalone_list(){
  local t owned=""
  # native_tests is only computed inside the native runner; derive it here so
  # the standalone stage can subtract exactly what that stage would run.
  NATIVE_LIST=""
  native_tests
  native_tests >/dev/null 2>&1 || true
  owned=" ${NATIVE_LIST:-} $SECURITY_TESTS_TABLE $CORE_TESTS_TABLE $API_TESTS_TABLE "
  owned="$owned tests/test_x509.js tests/test_crypto_aead.js "
  owned="$owned $(printf '%s\n' $MT_MULTI | tr ' ' '\n' | grep -v '^$' | tr '\n' ' ')"
  for t in $MT_STANDALONE_TABLE $STANDALONE_EXTRA; do
    case "$owned" in *" $t "*) continue ;; esac
    printf '%s ' "$t"
  done
}
g_stage_standalone(){
  _require_native_bin
  local list mo="${STANDALONE_TIMEOUT:-240}" rc=0 t sdir lf skipped=0
  sdir="$DEV_STATE/standalone-logs"; mkdir -p "$sdir"
  list=$(standalone_list)
  [ -n "$list" ] || { echo "standalone: no suite to run -- the table is empty?"; return 1; }
  echo "standalone: $(printf '%s\n' $list | wc -w | tr -d ' ') suites no other stage runs"
  local seen="" dedup="" f
  for f in $list; do
    case " $seen " in *" $f "*) continue ;; esac
    seen="$seen $f"
    case " $STANDALONE_HOST_SKIP " in
      *" $f "*)
        echo "  standalone SKIP  $f (dyna:uring is the Linux io_uring backend; real run: ./build.sh test-uring)"
        skipped=$((skipped+1)); continue ;;
    esac
    dedup="$dedup $f"
  done
  list="$dedup"
  echo "standalone: $(printf '%s\n' $list | wc -w | tr -d ' ') suites to run, $skipped host-skipped"
  for f in $list; do
    [ -f "$f" ] || { echo "standalone: FAIL -- $f is listed but does not exist"; rc=1; continue; }
    lf="$sdir/$(printf '%s' "$f" | tr '/' '_').log"
    # every run is EXTERNALLY bounded: a suite that declares no // timeout:
    # would otherwise be able to park this stage forever (B1-06.4).
    fl=$(head -3 "$f" | sed -n 's|^// *flags: *||p' | head -1)
    # shellcheck disable=SC2086
    BOUNDED_RUN_MARKER="$lf.bounded" \
      ./tools/bounded-run.sh "$mo" "$(( mo > 120 ? 120 : mo ))" "standalone:$f" -- \
      ./dynajs $fl "$f" >"$lf" 2>&1
    t=$?
    if [ "$t" = 0 ]; then printf '  standalone ok   %s\n' "$f"
    elif [ -f "$lf.bounded" ]; then
      echo "  standalone TIMEOUT $f (external ${mo}s bound)"; rc=1
    else
      echo "  standalone FAIL  $f (rc=$t)"; tail -12 "$lf" | sed 's/^/      /'; rc=1
    fi
  done
  [ "$rc" = 0 ] && echo "standalone: ok -- $(printf '%s\n' $list | grep -c . ) suites ran, $skipped host-skipped (logs in $sdir)"
  return "$rc"
}

# --- the guarantees stage (B1-16) -------------------------------------------
# Documented promises with no test that would go red. Each leg below is a
# promise the docs make and the gate now actually checks:
#   budget    --timeout-ms / --memory-limit / --native-memory-limit really
#              bound (tests/test_cli_budget_flags.sh, with the shapes that do
#              NOT bound pinned as known-broken so the gap cannot be lost)
#   help      every documented flag appears in --help (tools/check-help-flags.sh)
#   deps      the binary's dynamic dependency set (tools/check-binary-deps.sh)
#   runner    the parallel runner's verdict logic itself
#              (tests/runner-selftest/run.sh -- five stub suites, five distinct
#              failure shapes; without it the verdict rules were only ever read)
#   inputs    every gate-table path is tracked and present (B1-09's class)
#   lock      the per-tree build lock is owned, bounded and stale-detecting
g_stage_guarantees(){
  _require_dynajs
  local rc=0 leg
  for leg in inputs runner budget help deps lock; do
    case "$leg" in
      inputs) ./tools/check-gate-inputs.sh || rc=1 ;;
      runner) ./tests/runner-selftest/run.sh || rc=1 ;;
      budget) ./tools/bounded-run.sh 600 300 budget-flags -- \
                 ./tests/test_cli_budget_flags.sh || rc=1 ;;
      help)   ./tools/check-help-flags.sh ./dynajs || rc=1 ;;
      deps)   ./tools/check-binary-deps.sh ./dynajs || rc=1 ;;
      lock)   ./tools/bounded-run.sh 600 300 build-lock -- \
                 ./tests/test_build_lock.sh || rc=1 ;;
    esac
  done
  if [ "$rc" = 0 ]; then
    echo "guarantees: ok -- budget flags, --help coverage, binary deps, runner verdicts,"
    echo "             gate-table tracking and the build lock all proven"
  else
    echo "guarantees: FAILED (see the rows above)"
  fi
  return "$rc"
}

# --- the sanitizer leg (B1-10 / TEST-01) ------------------------------------
# Before this stage the gate ran NO sanitizer-instrumented binary at all: the
# asan/ubsan subcommands built without native modules (so every dyna:* import
# failed), and the ASan/UBSan C regression targets were reachable only by hand.
# A memory-lifetime regression could therefore pass the gate untouched.
#
# The leg is its own STAGE with its own artifact root (.obj-san): its objdir,
# its binaries and its lock are separate from the release tree, so it can never
# clobber (or be clobbered by) the binary the other stages are executing.
#
# Design decisions (recorded in CHANGELOG.md):
#  - ASan+UBSan together, with halt_on_error/abort_on_error, so the FIRST
#    report is the verdict instead of the Nth.
#  - LeakSanitizer: ON where the host supports it, OFF (and SAID to be off)
#    where it does not -- Darwin arm64 has no LSan at all.
#  - Wall-clock budget assertions are SCALED for this leg. A duration
#    threshold measured under -fsanitize=address,undefined measures the
#    sanitizer, not the engine; asserting the release-build number here would
#    be a coin flip. The engine-side fix (work-based bounds instead of
#    durations) is B1-15/TEST-03 and stays with the engine lane.
#  - Skippable with GATE_SAN=0 (it is the slowest leg); the skip is LOUD and
#    recorded, never a silent pass.
SAN_OUT="$ROOT/.obj-san"
# Suites known to trip a sanitizer on today's engine. Same UN-ROT rule as
# CTEST_XFAIL: pinned failures do not fail the gate, but a pin that starts
# PASSING fails the stage with XPASS so it can never outlive its bug.
#   tests/test_w3_parser.js  LeakSanitizer: 524296 bytes in 2 allocations from
#       vdup_hash_build() <- js_parse_check_duplicate_parameter()
#       (src/parser.inc.c:2359, called at :4768) when a function declares 60000
#       default parameters -- the SEC-084 row. Found by THIS stage: it is the
#       first sanitizer-instrumented run any gate ever made.
#       TICKET: B1-10/leak-vdup-hash-build.
SAN_XFAIL="tests/test_w3_parser.js"
SAN_CORE_LIST="tests/test_builtin.js tests/test_language.js tests/test_w3_parser.js tests/test_closure.js tests/test_bigint.js tests/test_textcodec.js tests/test_array_ext.js tests/test_string_ext.js tests/test_json.js tests/test_regexp_backtrack.js"
SAN_NATIVE_LIST="tests/test_sys.js tests/test_file.js tests/test_json.js tests/test_encoding.js tests/test_compress.js tests/test_csv.js tests/test_time.js tests/test_url.js tests/test_uuid.js tests/test_validate.js tests/test_xml.js tests/test_yaml.js tests/test_bytes.js tests/test_decimal.js tests/test_structures.js tests/test_mathx.js tests/test_random.js tests/test_simd.js tests/test_matcher.js tests/test_semver.js tests/test_hash_keyed.js tests/test_html.js tests/test_crypto.js tests/sec_dataframe_detach.js tests/test_audit_w2_engine_2.js tests/test_audit_w2_native_1.js tests/test_audit_w2_native_2.js tests/test_audit_w3_native_1.js tests/test_audit_w3_native_2.js tests/test_audit_w3_native_3.js tests/test_audit_w3_simd.js tests/test_audit_w3_mathx.js tests/test_audit_w3_native_4.js tests/test_audit_w3_native_5.js tests/test_audit_w3_native_6.js tests/test_audit_w3_native_7.js tests/test_audit_w3_native_8.js tests/test_audit_w3_native_9.js tests/test_audit_w3_native_10.js"
# LeakSanitizer availability is HOST-dependent and must be probed, not
# guessed: it is absent on some Darwin toolchains and present on others.
# SAN_LEAKS=1/0 forces it; auto asks tools/san-harness-check.sh, which plants a
# leak and reports whether LSan caught it.
san_leaks_setting(){
  case "${SAN_LEAKS:-auto}" in
    1) printf 'detect_leaks=1' ;;
    0) printf 'detect_leaks=0' ;;
    *) if [ -f "$SAN_OUT/.dev/san-harness/leak-support" ]; then
         printf 'detect_leaks=%s' "$(cat "$SAN_OUT/.dev/san-harness/leak-support")"
       else printf 'detect_leaks=0'; fi ;;
  esac
}
g_stage_san(){
  if [ "${GATE_SAN:-1}" = 0 ]; then
    echo "san: SKIPPED BY REQUEST (GATE_SAN=0) -- this leg is NOT proof that the"
    echo "     build is memory-clean; only an ASan/UBSan run can be that."
    return 0
  fi
  command -v clang >/dev/null 2>&1 || command -v gcc >/dev/null 2>&1 || {
    echo "san: no clang/gcc on this host -- cannot run the sanitizer leg"; return 1; }
  echo "san: proving the harness reports planted defects before trusting a clean run"
  ./tools/san-harness-check.sh "$SAN_OUT/.dev/san-harness" || {
    echo "san: the sanitizer harness is BLIND; a clean run below would prove nothing"; return 1; }

  local leaks san_opts san_envs
  leaks=$(san_leaks_setting)
  san_opts="halt_on_error=1:abort_on_error=1:detect_leaks=0"
  [ "$leaks" = detect_leaks=1 ] && san_opts="halt_on_error=1:abort_on_error=1:detect_leaks=1"
  san_envs="ASAN_OPTIONS=$san_opts UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1"
  echo "san: ASAN_OPTIONS=$san_opts"
  echo "     UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1"

  # own artifact root: separate objdir, separate binaries, separate lock
  local san_cfg="CONFIG_NATIVE_MODULES=y CONFIG_ASAN=y CONFIG_UBSAN=y $HARDEN_CFG"
  [ "$GATE_TLS" = y ] && san_cfg="CONFIG_NATIVE_MODULES=y CONFIG_TLS=y CONFIG_ASAN=y CONFIG_UBSAN=y $HARDEN_CFG"
  OUT="$SAN_OUT" DEV_BUILD_J="$GATE_J" g_run ./build.sh build $san_cfg || {
      echo "san: the ASan+UBSan build failed (diagnostics above)"; return 1; }
  local san_bin="$SAN_OUT/dynajs"
  [ -x "$san_bin" ] || { echo "san: no binary at $san_bin after a successful build"; return 1; }
  "$san_bin" -e 'import("dyna:csv")' >/dev/null 2>&1 || {
    echo "san: the sanitizer build cannot load dyna:* modules -- its suites would"
    echo "     SKIP and print green. Rebuild with CONFIG_NATIVE_MODULES=y."; return 1; }
  grep -q "__asan_init" <(nm "$san_bin" 2>/dev/null) || {
    echo "san: $san_bin carries NO AddressSanitizer instrumentation"; return 1; }
  grep -q "__ubsan_handle" <(nm "$san_bin" 2>/dev/null) || {
    echo "san: $san_bin carries NO UndefinedBehaviorSanitizer instrumentation"; return 1; }
  echo "san: instrumented engine ok ($san_bin)"

  # every suite invocation is externally bounded; a sanitizer report is a
  # report regardless of the suite's own exit code, so BOTH are checked.
  local scale="${SAN_TIME_SCALE:-8}" t mo="${SAN_SUITE_TIMEOUT:-300}" rc=0 f bad=0 ran=0 fl
  local xfail_bad=0
  local logdir="$SAN_OUT/.dev/san-logs"; mkdir -p "$logdir"
  local report
  report=$(printf '%s\n' $SAN_CORE_LIST $SAN_NATIVE_LIST $SECURITY_TESTS_TABLE \
            | tr ' ' '\n' | grep -v '^$' | sort -u)
  for f in $report; do
    [ -f "$f" ] || { echo "san: FAIL -- listed suite does not exist: $f"; rc=1; continue; }
    ran=$((ran+1))
    t="$logdir/$(printf '%s' "$f" | tr '/' '_').log"
    fl=$(head -3 "$f" | sed -n 's|^// *flags: *||p' | head -1)
    # the suites read their wall-clock budgets from these; a duration threshold
    # under a sanitizer measures the sanitizer, not the engine.
    # shellcheck disable=SC2086
    BOUNDED_RUN_MARKER="$t.bounded" \
    ./tools/bounded-run.sh "$mo" "$(( mo > 150 ? 150 : mo ))" "san:$f" -- \
        env $san_envs \
            ASAN_OPTIONS="$san_opts" \
            PEN_BUDGET_MS=$(( scale * 2000 )) \
            DYNAJS_BUDGET_MS=$(( scale * 1500 )) \
            TEST_BUDGET_MS=$(( scale * 2000 )) \
            "$san_bin" $fl "$f" "$scale" \
        >"$t" 2>&1
    local src=$?
    if grep -qE "ERROR: AddressSanitizer|ERROR: LeakSanitizer|runtime error:|SUMMARY: (Address|Undefined|Leak)Sanitizer" "$t"; then
      case " $SAN_XFAIL " in
        *" $f "*)
          echo "  san XFAIL  $f -- pinned, known-red on today's engine (see SAN_XFAIL in build.sh):"
          grep -m3 -E "ERROR: |runtime error:|SUMMARY: " "$t" | sed 's/^/      /'
          continue ;;
        *)
          echo "san: FAIL  $f -- a sanitizer REPORTED (exit $src)"
          grep -m3 -E "ERROR: |runtime error:|SUMMARY: " "$t" | sed 's/^/      /'
          bad=1; continue ;;
      esac
    fi
    if [ "$src" != 0 ]; then
      echo "san: FAIL  $f -- exit $src (bounded-run: $([ -f "$t.bounded" ] && echo yes || echo no))"
      tail -12 "$t" | sed 's/^/      /'
      bad=1; continue
    fi
    case " $SAN_XFAIL " in
      *" $f "*) echo "  san XPASS $f -- pinned as a KNOWN SANITIZER FAILURE but clean now;"
                  echo "           remove it from SAN_XFAIL in build.sh."; xfail_bad=1 ;;
      *)        printf '  san ok  %s\n' "$f" ;;
    esac
  done
  [ "$xfail_bad" != 0 ] && bad=1
  [ "$bad" = 1 ] && rc=1
  rm -f "$logdir"/*.bounded 2>/dev/null
  if [ "$rc" = 0 ]; then
    echo "san: $ran suites clean under ASan+UBSan (leaks=$leaks, time scale x$scale)"
    return 0
  fi
  echo "san: FAILED -- logs under $logdir"
  return 1
}

g_stage_rerun_hint(){
  case "$1" in
    coretest) echo "       context: DEV_JOBS=$(g_fan_cap) ./build.sh test-core TEST_SCOPE=all (PREPUSH_PAR=$GATE_PAR caps the fan-out)" ;;
    native)   if [ -n "${PREPUSH_MULTI_TESTS:-}" ]; then
                echo "       context: DEV_JOBS=$NCPU ./build.sh test-native MULTI_TESTS=$PREPUSH_MULTI_TESTS TEST_SCOPE=all"
              elif [ -n "${PREPUSH_MODULES:-}" ]; then
                echo "       context: DEV_JOBS=$NCPU ./build.sh test-native MODULES=\"$PREPUSH_MODULES\""
                echo "       (or: DEV_JOBS=$NCPU ./build.sh test-native TEST_SCOPE=all -- the full matrix)"
              else
                echo "       context: DEV_JOBS=$NCPU ./build.sh test-native TEST_SCOPE=all"
              fi ;;
    security) echo "       context: DEV_JOBS=$(g_fan_cap) ./build.sh test-security" ;;
    t262)     echo "       context: ./build.sh t262 (a subtree bisects faster: ./build.sh t262 builtins/)" ;;
    build)    echo "       context: ./build.sh clean && DEV_BUILD_J=$GATE_J ./build.sh build $NATIVE_CFG" ;;
    *)        : ;;
  esac
}
g_fail_stage(){
  g_say "FAIL: $1 (rc=$2) -- log: $GATE_LOGDIR/$1.log"
  if [ -f "$GATE_LOGDIR/$1.ok" ]; then
    g_say "       the leg ran for $(cat "$GATE_LOGDIR/$1.ok" 2>/dev/null || echo '?')s before failing"
  fi
  [ "$2" = 99 ] && g_say "       (no rc file was written -- the leg was killed hard (SIGKILL/OOM?) or never ran)"
  [ "$2" = 124 ] && g_say "       (stage was KILLED: exceeded PREPUSH_TIMEOUT=${GATE_TMO}s; raise it: PREPUSH_TIMEOUT=3600 ./build.sh gate)"
  local first
  first=$(grep -niE "error|assert|FAIL" "$GATE_LOGDIR/$1.log" 2>/dev/null | head -5)
  if [ -n "$first" ]; then
    g_say "       First findings (log line: text):"
    printf '%s\n' "$first" | sed 's/^/    /'
  fi
  tail -25 "$GATE_LOGDIR/$1.log" 2>/dev/null | sed 's/^/    /'
  g_stage_rerun_hint "$1"
  g_say "       re-run just this leg:  ./build.sh __gate-stage $1"
  g_say "       (flakes happen at roughly 1 run in 70 -- if the re-run is green, it was one)"
}

G_P_NAMES=(); G_P_IDX=()
g_pstart(){
  local name="$1"
  while [ "$(jobs -rp | wc -l)" -ge "$GATE_PAR" ]; do sleep 0.1; done
  G_P_NAMES+=("$name")
  GATE_STAGE_N=$((GATE_STAGE_N + 1)); G_P_IDX+=("$GATE_STAGE_N")
  g_say "  start [$GATE_STAGE_N/$GATE_STAGE_TOTAL] $name"
  if [ "${PREPUSH_DEBUG:-0}" = 1 ]; then
    g_say "[prepush] \$ ./build.sh __gate-stage $name  (log: $GATE_LOGDIR/$name.log)"
  fi
# Each leg gets its OWN TMPDIR and journal (B1-14: two legs sharing one
  # TMPDIR race on every scratch file the suites make there).
  mkdir -p "$GATE_LOGDIR/tmp/$name" 2>/dev/null
  (
    t0=$SECONDS
    TMPDIR="$ROOT/$GATE_LOGDIR/tmp/$name" \
    DEV_PROGRESS_LOG="$GATE_LOGDIR/tmp/$name/progress.log" \
    g_bound_gate bash "$ROOT/build.sh" __gate-stage "$name" >"$GATE_LOGDIR/$name.log" 2>&1
    rc=$?
    echo "$rc" > "$GATE_LOGDIR/$name.rc"
    echo $((SECONDS - t0)) > "$GATE_LOGDIR/$name.ok"
  ) &
}
g_pwait(){
  wait
  local bad=0 i n rc failed=""
  for i in "${!G_P_NAMES[@]}"; do
    n=${G_P_NAMES[$i]}
    rc=$(cat "$GATE_LOGDIR/$n.rc" 2>/dev/null || echo 99)
    if [ "$rc" = 0 ]; then
      printf '       [%d/%d] %s ok in %ss\n' "${G_P_IDX[$i]}" "$GATE_STAGE_TOTAL" "$n" "$(cat "$GATE_LOGDIR/$n.ok" 2>/dev/null || echo '?')"
    else
      bad=1; failed="$failed $n"
      g_fail_stage "$n" "$rc"
    fi
  done
  G_P_NAMES=(); G_P_IDX=()
  GATE_FAILED_LEGS="${failed# }"
  return "$bad"
}
g_bound_gate(){
  # the PREPUSH_TIMEOUT wall (build.sh's DEV_TIMEOUT governs the other runners).
  # No timeout binary is NOT a licence to run the stage unbounded: fall back to
  # tools/bounded-run.sh (pure shell, needs no external tool) and fail loudly
  # when even that is missing -- a gate that can hang silently proves nothing.
  if [ -n "${GATE_TIMEOUT_BIN:-}" ]; then "$GATE_TIMEOUT_BIN" -k 5 "$GATE_TMO" "$@"; return $?; fi
  if [ ! -x ./tools/bounded-run.sh ]; then
    echo "FAIL: gate: no timeout tool (timeout/gtimeout) and no tools/bounded-run.sh." >&2
    echo "      Refusing to run a gate stage UNBOUNDED: install coreutils (gtimeout)." >&2
    return 1
  fi
  ./tools/bounded-run.sh "$GATE_TMO" "$(( GATE_TMO > 600 ? 600 : GATE_TMO ))" "gate-stage" -- "$@"
}

gate_run(){
  # Scrub the ambient scoping knobs FIRST: this function runs from git hooks
  # and other parents whose exported MODULES/TEST_SCOPE must never silently
  # filter a stage the gate decided to run full. Scope arrives as ARGUMENTS
  # (MODULES=a,b / TEST_SCOPE=all / --full / positional names) or via
  # PREPUSH_MODULES.
  unset MODULES TEST_SCOPE
  local a scope_all=0 mods_arg="" smoke=()
  for a in "$@"; do
    case "$a" in
      TEST_SCOPE=all|--full) scope_all=1 ;;
      MODULES=*)             mods_arg="${a#MODULES=}" ;;
      CONFIG_*)              : ;;  # accepted for muscle memory; the gate's
                                   # build config is fixed ($NATIVE_CFG)
      *)
        # B1-06.10: a positional is a MODULE NAME or a FILE, never both.
        # `./build.sh gate csv` used to be both `MODULES=csv` and a smoke run
        # of the non-existent file `csv`, so the documented invocation failed
        # with "FAIL: smoke csv" after the whole gate had run.
        if [ -f "$a" ]; then smoke+=("$a")
        else mods_arg="${mods_arg:+$mods_arg,}$a"; fi ;;
    esac
  done
  mkdir -p "$GATE_LOGDIR" || return 2
  GATE_PAR=${PREPUSH_PAR:-3}
  GATE_TMO=${PREPUSH_TIMEOUT:-1800}
  GATE_J=${PREPUSH_J:-$NCPU}
  GATE_TIMEOUT_BIN=""
  if have timeout; then GATE_TIMEOUT_BIN=timeout
  elif have gtimeout; then GATE_TIMEOUT_BIN=gtimeout; fi
  GATE_FAILED_LEGS=""
  # 13 pre-existing stages (the 12-stage wave proof + the test262 leg) plus
  # ctest + standalone + guarantees + san. GATE_STAGES overrides the count.
  GATE_STAGE_TOTAL=${GATE_STAGES:-17}
  GATE_STAGE_N=0

  # --- resolve the scope once, export it for the __gate-stage children -------
  if [ "$scope_all" = 1 ]; then
    GATE_SCOPE=""; export GATE_SCOPE_ALL=1
  else
    export GATE_SCOPE_ALL=0
    if [ -n "$(printf '%s' "$mods_arg" | tr -d ' ,')" ]; then
      GATE_SCOPE=$(printf '%s' "$mods_arg" | tr ',' ' ' | tr -s ' ' | sed 's/^ //;s/ $//')
    elif [ -n "${PREPUSH_MODULES:-}" ]; then
      GATE_SCOPE=$(printf '%s' "$PREPUSH_MODULES" | tr ',' ' ' | tr -s ' ' | sed 's/^ //;s/ $//')
    else
      GATE_SCOPE=$(gate_scope_modules)
    fi
  fi
  export PREPUSH_MODULES="$GATE_SCOPE"

  g_say "================================================================="
  g_say "gate: $GATE_STAGE_TOTAL-stage proof gate, 2 waves (build -j$GATE_J, $GATE_PAR workers/wave, TLS: $( [ "$GATE_TLS" = y ] && echo yes || echo no))"
  if [ "$GATE_SCOPE_ALL" = 1 ]; then
    g_say "scope: TEST_SCOPE=all -- native+blackbox run the FULL matrix"
  else
    case " $GATE_SCOPE " in
      *" core "*)  g_say "scope: core touched -- native+blackbox run the FULL matrix" ;;
      *" infra "*) g_say "scope: infra touched -- native+blackbox run the FULL matrix" ;;
      "")          g_say "scope: no modules derived -- native+blackbox run the FULL matrix" ;;
      *)           g_say "scope: modules [$GATE_SCOPE] -- native+blackbox scoped (full matrix: TEST_SCOPE=all / --full / PREPUSH_MODULES=)" ;;
    esac
  fi
  g_say "note (--full): the gate's test262 leg ALWAYS runs the full $GATE_T262_TESTS-test corpus --"
  g_say "       expect ~30s wall (measured; bisect a regression with: ./build.sh t262 <subtree>)"
  g_say "expect: several minutes end to end (the core suite alone is ~4 min;"
  g_say "       per-stage cap ${GATE_TMO}s) -- logs land in $GATE_LOGDIR/<stage>.log"
  g_say "================================================================="

  local t0 brc pa_rc trc
  t0=$SECONDS

  # --- wave A: parse-only lints beside the clean+build ------------------------
  g_pstart codegraph
  g_pstart defects
  g_pstart imports
  GATE_STAGE_N=$((GATE_STAGE_N + 1))
  g_say "  start [$GATE_STAGE_N/$GATE_STAGE_TOTAL] build (clean + ./build.sh build -j$GATE_J)"
  g_stage_build; brc=$?
  g_pwait; pa_rc=$?
  if [ "$brc" != 0 ]; then g_fail_stage build "$brc"; g_say "       [build] FAIL"; return 1; fi
  g_say "       [$GATE_STAGE_N/$GATE_STAGE_TOTAL] build ok"
  if [ "$pa_rc" != 0 ]; then
    g_say "gate: FAIL (wave A lint;$GATE_FAILED_LEGS)"
    return 1
  fi

  # --- smoke: the [TEST.js...] semantics the gate has always kept -------------
  if [ ${#smoke[@]} -gt 0 ]; then
    for a in "${smoke[@]}"; do
      ./dynajs "$a" || { g_say "FAIL: smoke $a"; return 1; }
    done
    echo "smoke: ok"
  fi

  # --- the test262 leg ---------------------------------------------------------
  # B1-05: this leg BUILDS its runner (into its own OUT_BASE, so its own
  # objdir, binaries and lock) and only then runs it. It used to call
  # _t262_full against whatever binary happened to be lying in .obj-t262 --
  # which the gate never built -- so the conformance number came from a stale
  # binary and a fresh clone failed the leg with no message at all.
  # Serial: the runner's own -T eats every core.
  g_pstart t262
  if ! g_pwait; then
    g_say "gate: FAIL ($GATE_FAILED_LEGS)"
    g_say "gate: re-run the failed leg(s) with:  ./build.sh __gate-stage <name>"
    return 1
  fi

  # --- wave B: the suite legs on the worker pool ------------------------------
  # These all EXECUTE the one release ./dynajs the build leg produced and none
  # of them rebuilds it (the suite runners are probe-first), so sharing the
  # tree is safe. Stages that build, or write outside ./dynajs, are in the
  # serial waves below.
  g_pstart fuzz
  g_pstart coretest
  g_pstart native
  g_pstart api
  g_pstart security
  g_pstart repl
  g_pstart tls
  g_pstart blackbox
  if ! g_pwait; then
    g_say "gate: FAIL ($GATE_FAILED_LEGS)"
    g_say "gate: re-run the failed leg(s) with:  ./build.sh __gate-stage <name>"
    return 1
  fi

  # --- wave C: the serial legs -------------------------------------------------
  # B1-14. Each of these writes outside the shared ./dynajs, or binds fixed
  # ports / literal scratch paths another leg is using, so they run ALONE:
  #   ctest       compiles and runs the ASan/UBSan/TSan C regression targets
  #               (the only sanitizer-instrumented C proofs in the project)
  #   standalone  the MT_STANDALONE suites -- the HTTP server, the WebSocket
  #               server and the TLS server suites, none of which any other
  #               stage ran -- plus tests/test_tls_session.js
  #   san         builds and runs the ASan+UBSan engine in its own OUT_BASE
  for solo_stage in ctest standalone guarantees san; do
    if [ "$solo_stage" = san ] && [ "${GATE_SAN:-1}" = 0 ]; then
      GATE_STAGE_N=$((GATE_STAGE_N + 1))
      g_say "  start [$GATE_STAGE_N/$GATE_STAGE_TOTAL] san -- SKIPPED (GATE_SAN=0)"
      continue
    fi
    g_pstart "$solo_stage"
    if ! g_pwait; then
      g_say "gate: FAIL ($GATE_FAILED_LEGS)"
      g_say "gate: re-run the failed leg(s) with:  ./build.sh __gate-stage <name>"
      return 1
    fi
  done

  g_say "================================================================="
  g_say "gate: OK -- all $GATE_STAGE_TOTAL stages passed in $((SECONDS - t0))s"
  g_say "================================================================="
  return 0
}

usage(){
  cat <<'EOF'
build.sh -- one entry point for building, testing, proving and installing
dynascript. (dev.sh is a one-line shim that execs this file.) Terse output:
"<stage>: ok" or "FAIL: <why>", nonzero exit on any failure. The build is
NATIVE: config parsing, flags, harden probes, the dependency-tracked parallel
compile, links and codegen all run in this script, and so does every former
build-system target -- the test matrix, the slim/one minimal binaries, the
fuzz link proof, the check gates, install.

GLOBAL OPTIONS (before the subcommand; also SRC=/OUT= environment)
  --src DIR   build from DIR playing the role of src/ (workspace copies; the
              layout beneath it is unchanged: DIR/core, DIR/builtins, ...).
              Every engine reference honors it: the VPATH resolution
              (src -> src/core -> tools), the object inventory, the -I flags,
              the generated-source lookups.
  --out DIR   the artifact ROOT: objects, .d files, stamps, the binaries
              (dynajs/dynajsc/run-test262), the libraries AND the generated
              codegen files (repl.c/hello.c/test_fib.c -- never written into
              the source tree when --out is given). Variants nest beneath it
              as today (DIR/asan, DIR/fastdev, ...). Default keeps the
              historical layout byte-for-byte: src/ sources, .obj objects,
              repo-root binaries, repo-root generated C. src= and out= are
              part of the config stamp, so the same objdir can never mix
              objects from a different source tree or artifact root.

BUILD
  ./build.sh build [KEY=VALUE...]   0-warning hardened -Werror build
                                    (CONFIG_HARDEN=y CONFIG_WERROR=y default;
                                    pass any CONFIG_* the old build system
                                    took, e.g. CONFIG_NATIVE_MODULES=y
                                    CONFIG_TLS=y)
  ./build.sh build-fast             CONFIG_FASTDEV=y native -O0 build (.obj/fastdev)
  ./build.sh rebuild                clean + build
  ./build.sh run FILE [args]        build (incremental) then run a JS file
  ./build.sh clean                  remove objdirs + binaries + generated artifacts
  ./build.sh install [PREFIX]       build, strip, install bin+lib+headers
                                    (PREFIX default /usr/local; DESTDIR env honored)
  ./build.sh status                 toolchain, config stamp, derived scope
  ./build.sh ccheck [FILE]          single-TU compile check (all modified src/*.c)
  ./build.sh check-objs             the CONFIG_CHECK_JSVALUE check objects
  ./build.sh ws SUBCMD ...          concurrent workspaces (tools/ws.sh)
  ./build.sh amd64                  docker x86 SIMD verify

TEST
  ./build.sh test-native [SCOPE]    the native matrix. Requires a scope:
                                    TEST_SCOPE=all (deliberate full run),
                                    TEST_MODS=a,b (union of module lists) or
                                    MODULES=a,b (suites matching test_<m>/bb_<m>);
                                    bare, it FAILS with the derived module list.
                                    Ends with test-examples + the check gates.
  ./build.sh test [CONFIG_*...]     the core JS suite leg (test_builtin, the
                                    CORE_TESTS table, examples, --std legs)
  ./build.sh test-core              the core JS suites only (no examples)
  ./build.sh test-all               core JS leg + the full native matrix
  ./build.sh test-module M          the native suites scoped to one module
  ./build.sh t SUITE... [--case=PAT] [--list] [--bail] [--verbose]
                                    named suites against the EXISTING binary,
                                    never a build (refuses a stale binary).
                                    SUITE: a path, a name (csv -> tests/
                                    test_csv.js) or mod:<module>. --case runs
                                    only the matching cases of a tests/kit.js
                                    suite; /re/ is a regular expression.
  ./build.sh test-blackbox [f...]   tests/blackbox/run.sh; one run per bb_ filter
  ./build.sh quick [FILE...]        native build + core sanity suites (+ run FILEs)
  ./build.sh asan|ubsan|openlibm FILE|test   sanitizer/library build + run
  ./build.sh bench FILE [args]      CONFIG_NATIVE build + run (NO dyna:* modules)
  ./build.sh rss FILE [N...]        peak-RSS-plateau leak check
  ./build.sh t262 [SUBTREE]         run test262 (subtree, else full baseline check)

SLIM / ONE -- per-module minimal binaries
  ./build.sh mod [m1,m2...]         link slim-<m> for each module, run its
                                    module suites against it (default: all)
  ./build.sh one MODULE [--build-only] [--companions="objs"]
                                    the MINIMAL binary: MODDEFS_<m> (+ FILE iff
                                    dyna-file.o is linked) and the proven
                                    MINCOMPANION_<m> set
  ./build.sh slim-vars [CONFIG_*...] MOD=<m>   the variable dump tools/slim-prune.sh parses

FUZZ
  ./build.sh libfuzzer [CONFIG_*]   link every fuzz target (nat set needs
                                    CONFIG_NATIVE_MODULES=y)
  ./build.sh fuzz-audit             the gate/rule/source three-way audit
  ./build.sh fuzz-all               audit + link all + nm the sanitizer symbols
  ./build.sh fuzz-smoke [RUNS]      bounded run of each target over its corpus
  ./build.sh prepush-fuzz           the gate's link proof (audit + parallel link)
  ./build.sh fuzz_<name> [CONFIG_*] one target (fuzz_eval, fuzz_csv, fuzz_net, ...)

STANDALONE SUITES (the C regressions and single-file JS proofs)
  ./build.sh test-io-atomic test-io-slurp-cap test-regexp-prefilter
  ./build.sh test-dtoa-subnormal test-simd-bitmap test-ds-core test-ac-caps
  ./build.sh test-ds-btree-oom test-pool test-dns-codec test-resp-codec
  ./build.sh test-simd-tiers        element-wise kernels on this CPU's tier
  ./build.sh test-simd-x86          every kernel harness on SSE4.2 AND AVX2
                                    (Rosetta 2 on an Apple-silicon Mac)
  ./build.sh test-scram test-timer test-bc-read-safety test-aio-sendfile-busy
  ./build.sh test-aio-connect-offload test-dns-foreign-answers test-aio-disk
  ./build.sh test-exec-spawn test-crc32c-hw test-crc32c-race test-sha256-hw
  ./build.sh test-net-fdchurn test-net-udp-selfclose test-net-teardown-churn
  ./build.sh test-net-eyeballs-churn test-net-final-sweep test-net-udp-grave
  ./build.sh test-aio-tls
  ./build.sh test-uring-sendfile test-uring-connect-offload test-uring-tick
                                    (the three io_uring ones run in docker)
  ./build.sh test-uring             build the uring docker image and run it
  ./build.sh test-nofile            prove the no-dyna:file fallback
  plus the single-suite JS runners: test-x509 test-crypto-aead test-crypto-curve
  test-jwt-asym test-tls-server test-tls-matrix test-tls-session test-static-
  traversal test-html-pentest test-bytes-accessors test-crawl test-fetcher
  test-scrape-autoclient test-scrape-modern test-connect-resolve
  test-spawn-fdlimit
  ./build.sh test-security          the pentest/adversarial suites
  ./build.sh test-api               the five dyna:* API layers + surface/fuzz
  ./build.sh api-inventory          exports enumerated from the BINARY
  ./build.sh test-repl              the pty-driven REPL harness (python3)
  ./build.sh test-examples          every dyna:* example runs
  ./build.sh test-base58-alloc      the two exact-count allocation gates
  ./build.sh test-fixtures          tests/park_sweep.so
  ./build.sh bench-core|bench-core-tus|bench-ac      the C/JS benches
  ./build.sh oracle-dtoa|oracle-regexp               the differential oracles
  ./build.sh stats|microbench

CHECK GATES
  ./build.sh check                  core-purity + doc-lint + codegraph
  ./build.sh check-readme           README example blocks (-> check-install)
  ./build.sh check-install          install.sh brew+flow tests (-> check-api)
  ./build.sh check-api              examples/apps programs + anchors + error ids + types
  ./build.sh check-anchors|check-error-ids|check-types|check-dts-truth
  ./build.sh check-test-list        every named suite runs in some gate
  ./build.sh conformance            the pinned test262 figure, machine-readable

GATES
  ./build.sh gate [TEST.js...]      THE proof gate (tools/prepush-parallel.sh is
                                    absorbed here; `prepush` is an alias):
                                    13 legs, 2 waves -- codegraph/defects/
                                    imports lints beside the clean+build, the
                                    test262 FULL corpus (83,744 tests, ~30s
                                    wall), then fuzz/coretest/native/
                                    api/security/repl/tls/blackbox on a
                                    3-worker pool. Scope: bare -> derived
                                    (tools/affected-modules.sh); core/infra ->
                                    FULL, loudly; MODULES=a,b or positional
                                    names -> scoped legs; TEST_SCOPE=all or
                                    --full -> the full matrix. Logs land in
                                    .prepush/<stage>.log (+ .rc/.ok); re-run
                                    one leg: ./build.sh __gate-stage <name>
  ./build.sh prepush                alias of gate (what the pre-push hook runs)
  ./build.sh install-hooks          install the pre-push hook (it runs the gate)
  ./build.sh check-hooks            notice when the gate is not installed

PGO / SBOM / TEST262 BOOTSTRAP
  ./build.sh pgo                    train + merge + PGO_USE rebuild
  ./build.sh sbom                   CycloneDX 1.6 for the CURRENT configuration
  ./build.sh test2-bootstrap        clone/patch the test262 corpus
  ./build.sh test2|test2-default|test2-update|test2-check
  ./build.sh dyna-debug|run-test262-debug    -O0 debug-object binaries

Knobs: DEV_JOBS (test fan-out budget), DEV_BUILD_J (compile pool size, default
nproc), DEV_PAR (gate worker pool), DEV_TIMEOUT (per-stage wall seconds, 0
disables, default 1800), DEV_SERIAL=1 (one stage at a time, in-tree),
DEV_CCACHE=0 (disable ccache wrap), DEV_HARDEN_CFG (build-config string
appended to every build; default "CONFIG_HARDEN=y CONFIG_WERROR=y", set it to
"" to build unhardened), DEVSH_DEBUG=1 (echo every wrapped command + rc +
elapsed as "[devsh]" stderr lines), DEV_PROGRESS_LOG (stage journal, default
$OUT_BASE/.dev/progress.log), T262_BASELINE (full-t262 failure pin, default
58/83744), DEV_TREES (clone tree root, default $OUT_BASE/.dev/trees-$$),
DEV_STATE (private per-tree state root: journals, scratch, the build lock;
default $OUT_BASE/.dev -- never /tmp), DEV_LOCK_TIMEOUT (seconds to wait for
the per-tree build lock before failing, default 900), DEV_LOCK_STALE (age at
which a live-looking lock is broken anyway, default 3600; 0 disables),
MODULES (comma/space module list: scope test-native to those suites; the GATE
takes its scope as arguments instead -- see GATES), TEST_SCOPE=all (the
deliberate full-matrix escape), TEST_MODS=a,b (exact union
of module lists), MULTI_TESTS (override the cross-module list),
PREPUSH_MODULES/PREPUSH_TIMEOUT/PREPUSH_PAR/PREPUSH_J/PREPUSH_DEBUG/
PREPUSH_MULTI_TESTS (the gate's knobs),
FUZZ_NO_DEFAULT_SAN (opt out of the fuzz targets' default sanitizers),
FUZZ_SMOKE_RUNS (fuzz-smoke executions per target).

Exit codes / failure UX: rc=0 all stages ok; rc=1 first FAIL: (with diagnosis
hints: rc=124 = hit the DEV_TIMEOUT wall -- a hang, not a crash; rc=139 =
segfault, rerun './build.sh asan FILE' for a stack). Failed-stage logs are kept
and their path printed ("full log: ...").

Gotchas:
- asan/ubsan/openlibm/bench builds set NO CONFIG_NATIVE_MODULES: every dyna:*
  import fails or silently skips -- the script warns and prints the rebuild fix.
- switching configs auto-wipes that config's objdir (stamp .obj/.config-sig,
  the CONFIG_SIG port) and relinks the shared binaries (.build-variant).
- `gate` runs IN THIS TREE: its build leg is clean + build, so it must never
  run concurrently with your own build -- one .obj, one owner.
- TLS gate legs are SKIPPED, not failed, without OpenSSL >= 3 in pkg-config.
- test-native, test-api, test-security and the single-suite runners run the
  binary that is already built and PROBE its config instead of rebuilding: a
  rebuild in the wrong config would silently turn dyna:* cases into green skips.
EOF
}

# the NOTICE the old check-hooks target carried: a gate nobody installed is a
# gate nobody runs. A notice, never a failure -- a fresh clone must build.
_install_hooks_notice(){
  if test -d .git && grep -q 'dynajs-prepush-hook' .git/hooks/pre-push 2>/dev/null; then
    echo "check-hooks: pre-push gate installed"
  else
    echo ""
    echo "NOTICE: the pre-push gate is not installed. Run:  ./build.sh install-hooks"
    echo "        Without it nothing checks a push. See ./build.sh prepush."
    echo ""
  fi
  return 0
}
# the debug-object binaries: every object compiled at -O0 (mode debug) into the
# current objdir, linked once. Same shape as the two .debug rules.
_debug_bin(){
  local out="$1" kind="$2"
  eng_parse_cfg || return 1
  eng_detect
  eng_flags || return 1
  eng_objects
  mkdir -p "$OBJDIR" "$OBJDIR/examples" "$OBJDIR/tests"
  local objs_list b src obj specs="" objs=""
  case "$kind" in
    dynajs)      objs_list="dyna-cli repl $ENG_LIB_OBJS $ENG_NAT_OBJS" ;;
    run-test262) objs_list="run-test262 $ENG_LIB_OBJS" ;;
    *) echo "FAIL: _debug_bin: unknown kind $kind"; return 1 ;;
  esac
  for b in $objs_list; do
    src=$(eng_src_of "$b") || { echo "FAIL: no source for $b.c"; return 1; }
    obj="$OBJDIR/$b.debug.o"
    objs="$objs $obj"
    eng_stale "$obj" "$src" && specs+="$src"$'\t'"$obj"$'\t'"debug"$'\t'$'\n'
  done
  eng_pool "$specs" || return 1
  echo "  LINK $out"
  # shellcheck disable=SC2086
  $ENG_CC $ENG_LDFLAGS -o "${BIN_PFX}$out" $objs $ENG_LIBS
}

# ---- global options: --src / --out (before the subcommand; SRC=/OUT= env) ---
# --src DIR   the source tree playing the role of src/ (workspace copies); the
#             layout beneath it is unchanged (DIR/core, DIR/builtins, ...).
# --out DIR   the artifact root: objects, .d files, stamps, the binaries
#             (dynajs/dynajsc/run-test262), the libraries AND the generated
#             codegen files (repl.c/hello.c/test_fib.c) land there. Default
#             keeps today's layout byte-for-byte: src/ sources, .obj objects,
#             repo-root binaries, repo-root generated C.
SRC_DIR=${SRC:-src}
OUT_BASE=${OUT:-.obj}
OUT_BASE=${OUT_BASE%/}
while :; do
  case "${1:-}" in
    --src)  [ $# -ge 2 ] || { echo "FAIL: --src needs a directory argument" >&2; exit 1; }
            SRC_DIR=$2; shift 2 ;;
    --src=*) SRC_DIR="${1#--src=}"; shift ;;
    --out)  [ $# -ge 2 ] || { echo "FAIL: --out needs a directory argument" >&2; exit 1; }
            OUT_BASE=$2; shift 2 ;;
    --out=*) OUT_BASE="${1#--out=}"; shift ;;
    *) break ;;
  esac
done
[ -d "$SRC_DIR" ] || { echo "FAIL: --src '$SRC_DIR' is not a directory (paths are relative to $ROOT)" >&2; exit 1; }
# Path prefixes for the engine's outputs: "./" and "" keep the default layout's
# exact strings (./dynajs, repl.c); --out routes everything beneath OUT_BASE.
case "$OUT_BASE" in
  .obj) BIN_PFX="./"; GEN_PFX="";  STAMP_FILE=".build-variant" ;;
  *)    BIN_PFX="$OUT_BASE/"; GEN_PFX="$OUT_BASE/"; STAMP_FILE="$OUT_BASE/.build-variant" ;;
esac
# Private per-tree state: journals, scratch, the build lock. Never /tmp and
# never a fixed shared path (B1-07): two checkouts, or two users on one host,
# must not read each other's logs or lock each other out.
[ -n "${DEV_STATE:-}" ] || DEV_STATE="$OUT_BASE/.dev"
DEV_STATE=${DEV_STATE%/}
mkdir -p "$DEV_STATE" 2>/dev/null || DEV_STATE="$OUT_BASE/.dev"
[ -n "${PROGRESS_LOG:-}" ] || PROGRESS_LOG="$DEV_STATE/progress.log"
[ -n "${ENG_CONTRACT_LOG:-}" ] || ENG_CONTRACT_LOG="$DEV_STATE/contract/core.log"
TREES=${DEV_TREES:-$DEV_STATE/trees-$$}
export DEV_TREES="$TREES"
# scratch for _test_nofile_run / fuzz-smoke: inside the tree, per process
[ -n "${DEV_SCRATCH:-}" ] || DEV_SCRATCH="$DEV_STATE/scratch-$$"
mkdir -p "$DEV_SCRATCH" 2>/dev/null || DEV_SCRATCH="$DEV_STATE"
cmd="${1:-}"; shift 2>/dev/null || true
tree_lock_acquire "$@"
case "$cmd" in
  # ---- build engine (phase 1) ---------------------------------------------
  build)    engine_build $HARDEN_CFG "$@" || exit 1
            echo "build: ok (config:${ENG_CFG_DESC:+ $ENG_CFG_DESC})" ;;

  run)      [ $# -ge 1 ] || die "usage: run FILE [args]"; engine_build $HARDEN_CFG || exit 1
            _warn_dyna "$1"; _run_target "$@" ;;

  rebuild)  engine_clean
            echo "clean: ok"
            engine_build $HARDEN_CFG "$@" || exit 1
            echo "rebuild: ok" ;;

  build-fast) # The -O0 edit loop: CONFIG_FASTDEV gets its own OBJDIR in the
            # engine (.obj/fastdev), so -O0 objects can never link into an -O2
            # tree; the variant stamp still flips the ./dynajs binary. NOT for
            # measurement.
            engine_build CONFIG_NATIVE_MODULES=y CONFIG_FASTDEV=y $HARDEN_CFG "$@" || exit 1
            echo "build-fast: ok (-O0 CONFIG_FASTDEV=y + native modules; not a measured build)" ;;

  modules-changed) exec ./tools/affected-modules.sh "$@" ;;

  asan)     [ $# -ge 1 ] || die "usage: asan FILE|test"
            engine_build CONFIG_ASAN=y $HARDEN_CFG || exit 1
            _warn_dyna "$1"
            ASAN_OPTIONS=detect_leaks=0 _run_target "$@"; echo "asan: ok" ;;

  ubsan)    [ $# -ge 1 ] || die "usage: ubsan FILE|test"
            engine_build CONFIG_UBSAN=y $HARDEN_CFG || exit 1
            _warn_dyna "$1"
            UBSAN_OPTIONS=halt_on_error=1 _run_target "$@"; echo "ubsan: ok" ;;

  openlibm) [ $# -ge 1 ] || die "usage: openlibm FILE|test"
            [ -f third_party/openlibm/libopenlibm.a ] || \
              die "openlibm: third_party/openlibm/libopenlibm.a missing (fix: clone+build it)"
            engine_build CONFIG_OPENLIBM=y $HARDEN_CFG || exit 1
            _warn_dyna "$1"; _run_target "$@"; echo "openlibm: ok" ;;

  status)   echo "=== DynaJS Environment & Toolchain Status ==="
            echo "  Platform:     $(uname -s) $(uname -m)"
            echo "  CPU Cores:    $NCPU (compile pool: $JOBS)"
            echo "  src:          $SRC_DIR (override: --src DIR or SRC=)"
            echo "  out:          $OUT_BASE (override: --out DIR or OUT=; binaries: $BIN_PFX)"
            comp="none"
            if have clang; then comp="clang $(clang --version 2>/dev/null | head -1 | grep -oE '[0-9]+(\.[0-9]+)+' | head -1)";
            elif have gcc; then comp="gcc $(gcc --version 2>/dev/null | head -1 | grep -oE '[0-9]+(\.[0-9]+)+' | head -1)"; fi
            echo "  Compiler:     $comp"
            echo "  ccache:       $(have ccache && [ "$USE_CCACHE" != "0" ] && echo "enabled ($(ccache --version 2>/dev/null | head -1))" || echo "not enabled/found")"
            echo "  OpenSSL:      $(pkg-config --modversion openssl 2>/dev/null || echo "system/none")"
            echo "  SQLite:       $(pkg-config --modversion sqlite3 2>/dev/null || echo "not found")"
            echo "  libzstd:      $(pkg-config --modversion libzstd 2>/dev/null || echo "not found")"
            echo "  Last build:   $(cat "$OUT_BASE/.dev_cfg" 2>/dev/null || echo "clean/unbuilt")"
            echo "  Variant:      $(cat "$STAMP_FILE" 2>/dev/null || echo "none")"
            _derive_scope
            if [ -n "$MODS" ]; then
              echo "  Scope:        modules-changed -> [$MODS] ($SCOPE_REASON)"
            else
              echo "  Scope:        modules-changed -> full matrix ($SCOPE_REASON)"
            fi
            if [ "$GATE_TLS" = y ]; then
              echo "  Gate TLS:     on (OpenSSL >= 3.0 found; native gate builds carry CONFIG_TLS=y)"
            else
              echo "  Gate TLS:     off (no OpenSSL >= 3.0; TLS is built by NO gate -- brew install openssl@3)"
            fi
            if [ -x ./dynajs ]; then
              echo "  Binary:       ./dynajs ($(ls -lh ./dynajs | awk '{print $5}'))"
              echo "  Native Std:   $(./dynajs -e 'import("dyna:mathx").then(()=>print("available")).catch(()=>print("no"))' 2>/dev/null || echo "no")"
            else
              echo "  Binary:       not built"
            fi
            echo "  Debug mode:   DEVSH_DEBUG=$DEBUG"
            echo "status: ok" ;;

  check)    echo "=== Running Static Integrity Checks ==="
            ./tools/core-purity.sh || die "check: core-purity failed"
            ./tools/doc-lint.sh || die "check: doc-lint failed"
            if [ -f bench/codegraph.py ]; then
              log=$(mktemp)
              if python3 bench/codegraph.py . 10 >"$log" 2>&1; then
                echo "codegraph: ok"
              else
                echo "FAIL: codegraph (complexity/dependency violations:)"
                head -25 "$log" | sed 's/^/    /'
                echo "  full log kept at: $log"
                exit 1
              fi
              rm -f "$log"
            fi
            echo "check: ok" ;;

  ccheck)   eng_parse_cfg >/dev/null 2>&1 || true
            eng_detect
            local_cc="$ENG_CC"
            cdefs="-D_GNU_SOURCE -DCONFIG_NATIVE_MODULES -DCONFIG_TLS"
            [ "$(uname -s 2>/dev/null || echo unknown)" = Linux ] && cdefs="$cdefs -DCONFIG_IO_URING"
            cflags="-std=gnu17 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-missing-field-initializers $cdefs -I. -Isrc -Isrc/core"
            if [ $# -ge 1 ]; then
              echo "ccheck: $1"
              $local_cc $cflags -fsyntax-only "$1" || die "ccheck $1 (single-TU compile errors above)"
            else
              files=$(git diff --name-only HEAD 2>/dev/null | grep -E '^src/.*\.c$' | grep -v '\.inc\.c$' || true)
              if [ -z "$files" ]; then echo "ccheck: no modified src/*.c (pass FILE=src/foo.c)";
              else
                for f in $files; do echo "ccheck: $f"; $local_cc $cflags -fsyntax-only "$f" || die "ccheck (compile errors above)"; done
              fi
            fi
            echo "ccheck: ok" ;;

  check-objs) engine_check_objs "$@" && echo "check-objs: ok" ;;

  __cc-lines) _cc_lines_run "$@" ;;
  __cc-line)  # $@ = [CONFIG_* ...] OBJ
            cc_obj=""; cc_cfg=""
            for cc_a in "$@"; do
              case "$cc_a" in CONFIG_*) cc_cfg="$cc_cfg $cc_a" ;; *) cc_obj="$cc_a" ;; esac
            done
            [ -n "$cc_obj" ] || die "__cc-line: pass the object basename (e.g. .obj/dyna-ml.o -> dyna-ml)"
            cc_obj="${cc_obj%.o}"; cc_obj="${cc_obj##*/}"
            # shellcheck disable=SC2086
            eng_parse_cfg $cc_cfg >/dev/null 2>&1 || true
            eng_detect; eng_flags || exit 1; eng_objects
            __cc_line_of "$cc_obj" ;;

  ws)       [ $# -ge 1 ] || die "usage: ws create|list|diff|merge|reset|drop|shell ...";
            ./tools/ws.sh "$@" ;;

  quick)    # NATIVE_CFG (native modules + TLS when available), not the bare
            # native config: tests/test_crypto.js gates RSA on the OpenSSL
            # (CONFIG_TLS) build, so the historical bare config failed here.
            engine_build $NATIVE_CFG || exit 1
            echo "running core sanity test suite..."
            ./dynajs --std tests/test_builtin.js || die "quick: ./dynajs --std tests/test_builtin.js (rc above)"
            ./dynajs tests/test_language.js || die "quick: ./dynajs tests/test_language.js (rc above)"
            ./dynajs tests/test_encoding.js || die "quick: ./dynajs tests/test_encoding.js (rc above)"
            ./dynajs tests/test_json.js || die "quick: ./dynajs tests/test_json.js (rc above)"
            ./dynajs tests/test_crypto.js || die "quick: ./dynajs tests/test_crypto.js (rc above)"
            if [ $# -gt 0 ]; then
              for t in "$@"; do need_file "$t"; ./dynajs "$t" || die "run $t (rc above)"; done
            fi
            echo "quick: ok" ;;

  bench)    [ $# -ge 1 ] || die "usage: bench FILE [args]"
            engine_build CONFIG_NATIVE=y $HARDEN_CFG || exit 1; need_file "$1"
            _warn_dyna "$1"
            _v ./dynajs "$@" \
              || die "bench: ./dynajs $* exited rc=$? (CONFIG_NATIVE=y has no dyna:* modules -- use quick for those)" ;;

  t262)     # isolated build: the t262 runner needs no dyna:* modules, and a
            # default-config rebuild here would clobber the developer's
            # native ./dynajs (the recurring config-clobber trap).
            T262_BIN="$ROOT/.obj-t262/run-test262"
            OUT="$ROOT/.obj-t262" "$0" build $HARDEN_CFG || exit 1
            [ -x "$T262_BIN" ] || die "t262: runner missing at $T262_BIN"
            if [ $# -ge 1 ]; then
              [ -f "$CONF" ] || die "$CONF missing (bootstrap test262/ first)"
              d="$1"; [ -d "$d" ] || d="test262/test/$1"
              [ -d "$d" ] || die "no such test262 subtree: $1 (pass a path under test262/test)"
              "$T262_BIN" -c "$CONF" -a -T "$JOBS" -d "$d" 2>&1 | grep -E "^Result:" || die "test262 subtree $1 (no Result line)"
            else _t262_full "$T262_BIN"; fi ;;

  rss)      [ $# -ge 1 ] || die "usage: rss FILE [N...]"; f="$1"; shift
            need_file "$f"; engine_build $HARDEN_CFG || exit 1
            [ $# -ge 1 ] || set -- 20000 100000 500000
            for N in "$@"; do
              if have /usr/bin/time && /usr/bin/time -l true >/dev/null 2>&1; then
                r=$(/usr/bin/time -l ./dynajs "$f" "$N" 2>&1 | awk '/maximum resident/{print $1}')
              else
                r=$(/usr/bin/time -v ./dynajs "$f" "$N" 2>&1 | awk -F': ' '/Maximum resident/{print $2"K"}')
              fi
              echo "N=$N peakRSS=${r:-?}"
            done
            echo "rss: flat across N => no leak" ;;

  amd64)    have docker || die "amd64: docker not installed (brew install --cask docker)"
            _v bash docker/build-and-test.sh amd64 \
              || die "amd64: docker/build-and-test.sh amd64 failed (is Docker running? docker info; full output above)"
            echo "amd64: ok" ;;

  # ---- suite runners (phase 2) ----------------------------------------------
  test)     # the core JS suite leg: test_builtin, the CORE_TESTS table, the
            # compiled examples, the --std handler legs and (when this config
            # builds .so modules) bjson/point. Module args are MANDATORY (or
            # TEST_SCOPE=all): the matrix never runs implicitly.
            _split_run_args mods "$@"
            if [ -z "$P2_SCOPE" ]; then _scope_refusal "test"; fi
            _core_scope_list "$P2_SCOPE"
            if [ "$CORE_FULL" != 1 ] && [ -z "$CORE_LIST" ]; then
              echo "test: no core-leg suite matches [${P2_SCOPE#MODULES=}] -- the engine-core table has no such module."
              echo "      scoped coverage for module work lives in: ./build.sh test-native ${P2_SCOPE}"
              exit 1
            fi
            # probe-first, exactly like the old default target: the suite leg
            # runs the binary that is already built and only builds when none
            # is present (a rebuild here would silently swap configs under a
            # parallel gate). Explicit CONFIG_* still pin the config.
            if [ -n "$P2_CFG" ]; then engine_build $P2_CFG || exit 1
            elif [ ! -x ./dynajs ]; then engine_build $HARDEN_CFG || exit 1
            else _config_ctx; fi
            echo "test: $([ "$CORE_FULL" = 1 ] && echo "full core leg (TEST_SCOPE=all)" || echo "scoped to [${P2_SCOPE#MODULES=}]")"
            _test_recipe || exit 1
            _install_hooks_notice
            echo "test: ok" ;;

  test-core)
            # the engine-core JS suites only: no examples, no native/bb legs.
            # probe-first (see `test`): runs the existing binary
            _split_run_args "$@"
            if [ -n "$P2_CFG" ]; then engine_build $P2_CFG || exit 1
            elif [ ! -x ./dynajs ]; then engine_build $HARDEN_CFG || exit 1
            else _config_ctx; fi
            _core_tests_run || exit 1
            echo "test-core: ok" ;;

  test-all) # core JS leg + native matrix, under ONE mandatory scope: the full
            # run is TEST_SCOPE=all ONLY -- positional names scope both legs
            _split_run_args mods "$@"
            if [ -z "$P2_SCOPE" ]; then _scope_refusal "test-all"; fi
            _core_scope_list "$P2_SCOPE"
            if [ -n "$P2_CFG" ]; then engine_build $P2_CFG || exit 1
            elif [ ! -x ./dynajs ]; then engine_build $NATIVE_CFG || exit 1
            else _config_ctx; fi
            if [ "$CORE_FULL" = 1 ]; then
              echo "test-all: full core leg + full native matrix (TEST_SCOPE=all)"
              _test_recipe || exit 1
            elif [ -z "$CORE_LIST" ]; then
              echo "test-all: no core-leg suite matches [${P2_SCOPE#MODULES=}] -- skipping the core leg (scoped coverage lives in the native matrix)"
            else
              echo "test-all: core leg scoped to [${P2_SCOPE#MODULES=}]"
              _test_recipe || exit 1
            fi
            _test_native_run ${P2_CFG:+$P2_CFG }$P2_SCOPE || exit 1
            echo "test-all: ok" ;;

  test-native)
            _split_run_args mods "$@"
            _test_native_run ${P2_CFG:+$P2_CFG }$P2_SCOPE ;;

  test-blackbox)
            engine_build $NATIVE_CFG || exit 1
            if [ $# -ge 1 ]; then
              for filt in "$@"; do
                # run.sh exits 0 even when a filter matches zero suites -- a
                # silent pass reads as coverage, so check the match here first
                # (same substring-of-the-basename rule run.sh applies)
                ls tests/blackbox/*"$filt"*.js >/dev/null 2>&1 \
                  || die "test-blackbox: no tests/blackbox/*${filt}*.js matches (try: ls tests/blackbox)"
                _v tests/blackbox/run.sh ./dynajs "$filt" \
                  || die "test-blackbox: filter '$filt' failed (rc above)"
              done
            else
              _v tests/blackbox/run.sh ./dynajs || die "test-blackbox failed (rc above)"
            fi
            echo "test-blackbox: ok" ;;

  test-module|test-mod)
            [ $# -eq 1 ] || die "usage: test-module MODULE  (exactly one module, e.g. test-module csv)"
            case " $MT_KNOWN_MODS " in *" $1 "*) ;; *) die "unknown module: $1 (known: $MT_KNOWN_MODS)" ;; esac
            _test_native_run "MODULES=$1" ;;
  t)        _t_run "$@" ;;
  mod)      _mod_run "$@" ;;

  one)      _one_run "$@" ;;

  slim-vars) _slim_vars_run "$@" ;;

  # ---- fuzz -----------------------------------------------------------------
  libfuzzer)
            fz_setup "$@" || exit 1
            fz_build $FZ_RULES || exit 1
            if [ -n "$(cfg_v NATIVE_MODULES)" ]; then fz_build $FZ_NAT_RULES || exit 1; fi
            echo "libfuzzer: ok" ;;
  fuzz-audit)
            fz_setup "$@" || exit 1
            fz_audit_run && fz_tls_link_audit ;;
  tls-link-audit) fz_tls_link_audit ;;
  fuzz-all) fz_all_run "$@" ;;
  fuzz-smoke)
            fz_setup "$@" || exit 1
            fz_smoke_run ;;
  prepush-fuzz) fz_prepush_run ;;
  fuzz_eval|fuzz_compile|fuzz_regexp|fuzz_regexp_compile|fuzz_json|fuzz_bytecode|\
  fuzz_bceval|fuzz_module_export)
            fz_setup "$@" || exit 1
            fz_build "$cmd" || exit 1
            echo "$cmd: ok" ;;
  # the nat targets' very existence was config-gated: they link the native
  # module objects, so the native config is forced (their own recipes say so)
  fuzz_dataframe|fuzz_stdlib|fuzz_parsers|fuzz_oauth2)
            fz_setup CONFIG_NATIVE_MODULES=y "$@" || exit 1
            fz_build "$cmd" || exit 1
            echo "$cmd: ok" ;;
  fuzz_codec|fuzz_dyns|fuzz_lz4|fuzz_scram|fuzz_net)
            fz_setup "$@" || exit 1
            fz_direct_one "$cmd" || exit 1
            echo "$cmd: ok" ;;
  fuzz_csv)
            fz_setup CONFIG_NATIVE_MODULES=y "$@" || exit 1
            _fz_archive libdynajs.fuzz.a $ENG_LIB_OBJS || exit 1
            fz_direct_one "$cmd" || exit 1
            echo "$cmd: ok" ;;

  # ---- standalone C regression targets --------------------------------------
  test-io-atomic|test-io-slurp-cap|test-regexp-prefilter|test-dtoa-subnormal|\
  test-simd-bitmap|test-ds-core|test-ac-caps|test-ds-btree-oom|test-pool|\
  test-dns-codec|test-resp-codec|test-scram|test-timer|test-bc-read-safety|\
  test-stack-limit|\
  test-aio-sendfile-busy|test-aio-connect-offload|test-dns-foreign-answers|\
  test-aio-disk|test-exec-spawn|test-uring-sendfile|test-uring-connect-offload|\
  test-uring-tick|test-crc32c-hw|test-crc32c-race|test-sha256-hw|\
  test-net-fdchurn|test-net-udp-selfclose|test-net-teardown-churn|test-net-eyeballs-churn|\
  test-simd-f64|test-simd-int|test-simd-reductions|test-text-kernels|test-utf16-kernels|\
  test-simd-tiers|test-simd-x86|test-socket-cloexec|test-evloop-cloexec|\
  bench-core-tus|bench-ac)
            _c_regression_targets "$cmd" || exit 1
            echo "$cmd: ok" ;;
  # these three link the CONFIGURED engine, so their objects must exist and
  # be fresh: build the config they will link against first
  test-aio-tls|test-net-final-sweep|test-net-udp-grave)
            _split_run_args "$@"
            if [ -n "$P2_CFG" ] || [ -n "${CONFIG_NATIVE_MODULES:-}" ]; then
              engine_build $P2_CFG || exit 1
            else
              engine_build $NATIVE_CFG || exit 1
            fi
            eng_objects
            _c_regression_targets "$cmd" || exit 1
            echo "$cmd: ok" ;;

  # ---- pure-JS single-suite runners (never rebuild: the probe is the point) --
  test-crypto-aead)     _js_suite tests/test_crypto_aead.js tests/test_crypto_aead.js ;;
  test-static-traversal) _js_suite tests/test_static_traversal.js --std tests/test_static_traversal.js ;;
  test-spawn-fdlimit)   _require_dynajs
                        sh -c 'ulimit -n 64 && exec ./dynajs --std tests/test_spawn_fdlimit.js' \
                          || { echo "FAIL: test-spawn-fdlimit"; exit 1; }
                        echo "test-spawn-fdlimit: ok" ;;
  test-html-pentest)    _js_suite tests/test_html_pentest.js tests/test_html_pentest.js ;;
  test-bytes-accessors) _js_suite tests/test_bytes_accessors.js tests/test_bytes_accessors.js ;;
  test-tls-server)      _js_suite tests/test_tls_server.js --std tests/test_tls_server.js ;;
  test-tls-matrix)      _js_suite tests/test_tls_matrix.js --std tests/test_tls_matrix.js ;;
  test-tls-session)     _js_suite tests/test_tls_session.js tests/test_tls_session.js ;;
  test-crawl)           _js_suite tests/test_scrape_crawl.js tests/test_scrape_crawl.js ;;
  test-fetcher)         _js_suite tests/test_scrape_fetcher.js tests/test_scrape_fetcher.js ;;
  test-scrape-autoclient) _js_suite tests/test_scrape_autoclient.js tests/test_scrape_autoclient.js ;;
  test-scrape-modern)   _js_suite tests/test_scrape_modern.js tests/test_scrape_modern.js ;;
  test-connect-resolve) _js_suite tests/test_connect_resolve.js tests/test_connect_resolve.js ;;
  test-jwt-asym)        _js_suite tests/test_jwt_asym.js tests/test_jwt_asym.js ;;
  test-crypto-curve)    _js_suite tests/test_crypto_curve.js tests/test_crypto_curve.js ;;
  test-x509)            _js_suite tests/test_x509.js tests/test_x509.js ;;

  test-security)
            _require_native_bin
            _run_parallel $SECURITY_TESTS_TABLE || { echo "FAIL: test-security"; exit 1; }
            echo "test-security: all suites passed" ;;
  test-api)
            _require_native_bin
            t=""; fl=""
            for t in $API_TESTS_TABLE; do
              [ -f "$t" ] || { echo "FAIL: $t is listed but missing"; exit 1; }
              echo "=== $t"
              fl=$(head -3 "$t" | grep -E '^// *flags:' | head -1 | sed -E 's|^//[[:space:]]*flags:||')
              # shellcheck disable=SC2086
              ./dynajs $fl "$t" || { echo "FAIL: $t"; exit 1; }
            done
            echo "test-api: all API suites passed" ;;
  api-inventory)
            _require_dynajs
            ./dynajs tools/api-inventory.js ;;
  test-repl)
            _require_dynajs
            if ! have python3; then echo "=== SKIP test-repl: python3 not found, the pty harness needs it"; exit 0; fi
            python3 tests/test_repl.py --binary ./dynajs || { echo "FAIL: test-repl"; exit 1; }
            echo "test-repl: ok" ;;
  test-examples)
            _test_examples_run || exit 1
            echo "test-examples: ok" ;;
  test-base58-alloc)
            _require_dynajs
            tests/run_base58_alloc.sh ./dynajs  || { echo "FAIL: tests/run_base58_alloc.sh"; exit 1; }
            tests/run_base58_strict.sh ./dynajs || { echo "FAIL: tests/run_base58_strict.sh"; exit 1; }
            echo "test-base58-alloc: ok" ;;
  test-fixtures)
            _split_run_args "$@"
            if [ -n "$P2_CFG" ] || [ -n "${CONFIG_NATIVE_MODULES:-}" ]; then
              engine_build $P2_CFG || exit 1
            else
              engine_build $NATIVE_CFG || exit 1
            fi
            eng_objects
            _park_sweep_so || exit 1
            echo "test-fixtures: ok" ;;
  bench-core)
            _require_dynajs
            ./dynajs --std tests/bench_parse_corpus.js || exit 1
            ./dynajs tests/bench_regexp.js || exit 1
            ./dynajs tests/bench_numeric.js || exit 1
            ./dynajs --std tests/bench_stdio.js || exit 1
            if ./dynajs -e 'import("dyna:sys")' >/dev/null 2>&1; then
              ./dynajs --std tests/bench_regexp_memory.js || exit 1
            else
              echo "=== SKIP bench_regexp_memory: needs CONFIG_NATIVE_MODULES=y for dyna:sys"
            fi
            if ls bench/frameworks/*.js >/dev/null 2>&1; then
              ./dynajs --std tests/bench_parse_frameworks.js || exit 1
            else
              echo "=== SKIP bench_parse_frameworks: no corpus in bench/frameworks/"
              echo "===      fetch it with tests/fetch_frameworks.sh"
            fi
            echo "bench-core: ok" ;;
  oracle-dtoa)  _require_dynajs; ./dynajs tests/oracle_dtoa.js || { echo "FAIL: oracle-dtoa"; exit 1; }; echo "oracle-dtoa: ok" ;;
  oracle-regexp) _require_dynajs; ./dynajs tests/oracle_regexp_fuzz.js 100000 || { echo "FAIL: oracle-regexp"; exit 1; }; echo "oracle-regexp: ok" ;;
  stats)        _require_dynajs; ./dynajs -qd; echo "stats: ok" ;;
  microbench)   _require_dynajs; ./dynajs --std tests/microbench.js; echo "microbench: ok" ;;

  # ---- check-* gates ---------------------------------------------------------
  check-readme)   _require_dynajs; _check_readme_run || exit 1 ;;
  check-install)  _require_dynajs; _check_install_run || exit 1 ;;
  check-api)      _require_dynajs; _check_api_run || exit 1 ;;
  check-anchors)  _check_anchors_run || exit 1 ;;
  check-error-ids) ( eng_parse_cfg >/dev/null 2>&1 || true; eng_detect; _check_error_ids_run ) || exit 1 ;;
  check-types)    _check_types_run || exit 1 ;;
  check-dts-truth) _check_dts_truth_run || exit 1 ;;
  check-test-list) _check_test_list_run || exit 1 ;;
  check-hooks)    _install_hooks_notice ;;
  conformance)    _conformance_run || exit 1 ;;

  # ---- install / pgo / sbom / test262 ---------------------------------------
  install)      _install_run "$@" ;;
  install-hooks) _install_hooks_run || exit 1 ;;
  pgo)          _pgo_run ;;
  sbom)         python3 tools/gen-sbom.py --timestamp "$(date -u +%Y-%m-%dT%H:%M:%SZ)" ;;
  test2)          _test2_run -t -m -c tools/test262.conf -a ;;
  test2-default)  _test2_run -t -m -c tools/test262.conf ;;
  test2-update)   _test2_run -t -u -c tools/test262.conf -a ;;
  test2-check)    _test2_run -t -m -c tools/test262.conf -E -a ;;
  test2-bootstrap) _test2_bootstrap_run ;;
  test-nofile)  _test_nofile_run ;;
  test-uring)   _test_uring_run ;;

  dyna-debug)         _debug_bin dyna-debug dynajs || exit 1 ;;
  run-test262-debug)  _debug_bin run-test262-debug run-test262 || exit 1 ;;

  # ---- the gate and the prepush proof ---------------------------------------
  # `gate` IS the proof gate now (see gate_run above): the 12-stage wave proof
  # absorbed from tools/prepush-parallel.sh plus the test262 corpus leg.
  # Positional names stay module selectors AND smoke TEST.js files:
  #   ./build.sh gate csv          == ./build.sh gate MODULES=csv
  #   ./build.sh gate tests/t.js   scopes the matrix AND smoke-runs the file
  gate)     gate_run "$@" ;;
  prepush)  gate_run "$@" ;;   # alias: the name the pre-push hook keeps using

  clean)    engine_clean; echo "clean: ok" ;;

  __stage)  _stage "$@"; exit $? ;;
  __t262)   _t262_full; exit $? ;;
  __gate-stage)
            # one proof-gate leg, in this tree (suite legs need the build the
            # gate produced); the same scrub the gate itself does
            unset MODULES TEST_SCOPE
            GATE_PAR=${PREPUSH_PAR:-3}
            GATE_TMO=${PREPUSH_TIMEOUT:-1800}
            GATE_J=${PREPUSH_J:-$NCPU}
            GATE_TIMEOUT_BIN=""
            if have timeout; then GATE_TIMEOUT_BIN=timeout
            elif have gtimeout; then GATE_TIMEOUT_BIN=gtimeout; fi
            name="${1:-?}"
            case "$name" in
              codegraph|defects|imports|build|t262|fuzz|coretest|tls|native|\
              api|security|repl|blackbox|san|ctest|standalone|guarantees) ;;
              *) die "unknown gate stage: $name (valid: codegraph defects imports build t262 fuzz coretest native api security repl tls blackbox san ctest standalone guarantees)" ;;
            esac
            "g_stage_$name"
            exit $? ;;

  ""|-h|--help|help)
            usage ;;

  *)        die "unknown command: $cmd (try ./build.sh help)" ;;
esac
