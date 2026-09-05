#!/usr/bin/env bash
# dev.sh — one entry point for building, testing and profiling dynascript, so we
# stop hand-writing shell. Output is terse: "<stage>: ok" or "FAIL: <why>" and a
# nonzero exit on any failure. Run `./dev.sh` for the command list.
#
#   ./dev.sh build [MAKEARGS...]     0-warning build (fails on any warning/error)
#   ./dev.sh mod csv,net     just those modules' tests via a slim binary
#   ./dev.sh ccheck [FILE]   single-TU compile check (all modified src/*.c)
#   ./dev.sh ws create NAME   concurrent workspace (build/test isolated; ws diff|merge to converge)
#   ./dev.sh run   FILE [args...]    build (incremental) then run a JS file
#   ./dev.sh test                    make test
#   ./dev.sh asan  FILE|test         ASan build + run (auto `make clean` on cfg switch)
#   ./dev.sh ubsan FILE|test         UBSan build + run
#   ./dev.sh t262  [SUBTREE]         run test262 (subtree, else full baseline check)
#   ./dev.sh bench FILE [args...]    CONFIG_NATIVE build + run
#   ./dev.sh status                  display toolchain, ccache, dependencies & build info
#   ./dev.sh check                   run static purity, doc-lint, and codegraph checks
#   ./dev.sh rss   FILE [N...]       peak-RSS-plateau leak check (FILE reads scriptArgs[1]=N)
#   ./dev.sh openlibm FILE|test      CONFIG_OPENLIBM build + run
#   ./dev.sh amd64                   docker x86 SIMD verify (Dockerfile target: amd64)
#   ./dev.sh gate  [TEST.js...]      full proof: 0-warn + ASan + UBSan + make test + test-api + test-security + test262
#   ./dev.sh clean
#
# Knobs: DEV_JOBS (core budget), DEV_BUILD_J (-j per single build), DEV_GATE_BUILD_J
# (-j per parallel gate stage), DEV_PAR (concurrent stages), DEV_TIMEOUT (per-stage
# wall seconds, 0 disables), DEV_SERIAL=1 (one stage at a time), DEV_CCACHE=1 (use
# ccache when available), DEV_PROGRESS_LOG (/tmp/devsh_progress.log).
set -uo pipefail
cd "$(dirname "$0")" || exit 2
ROOT=$PWD

NCPU=$( (command -v nproc >/dev/null 2>&1 && nproc) || sysctl -n hw.ncpu 2>/dev/null || echo 4 )
JOBS=${DEV_JOBS:-$NCPU}
# Single-target builds use full core parallelism; parallel gate stages cap at -j3
# to prevent CPU oversubscription across concurrent tree builds.
BUILD_J=${DEV_BUILD_J:-$JOBS}
GATE_BUILD_J=${DEV_GATE_BUILD_J:-3}
PAR=${DEV_PAR:-$(( JOBS / GATE_BUILD_J > 1 ? JOBS / GATE_BUILD_J : 1 ))}
TIMEOUT=${DEV_TIMEOUT:-1800}
SERIAL=${DEV_SERIAL:-0}
USE_CCACHE=${DEV_CCACHE:-1}
PROGRESS_LOG="${DEV_PROGRESS_LOG:-/tmp/devsh_progress.log}"

BASELINE="${T262_BASELINE:-58/83744}"
CONF=tools/test262.conf
STAMP=.obj/.dev_cfg
TREES=${DEV_TREES:-/tmp/build$$}
export DEV_TREES="$TREES"

# The TLS stack was compiled by NO gate: the native build does not set
# CONFIG_TLS, so dyna-tls.c, HTTPS and the AEAD ciphers were never built or
# run while asan/ubsan reported ok. Detect OpenSSL >= 3.0 the same way the
# Makefile's CONFIG_TLS block does and gate WITH it when present.
OPENSSL_PC="$(brew --prefix openssl@3 2>/dev/null || brew --prefix openssl 2>/dev/null)/lib/pkgconfig"
GATE_TLS=""
PKG_CONFIG_PATH="$OPENSSL_PC:${PKG_CONFIG_PATH:-}" \
  pkg-config --exists 'openssl >= 3.0' 2>/dev/null && GATE_TLS="y"
# Every native stage carries the same TLS decision as the tree it runs in.
NATIVE_CFG="CONFIG_NATIVE_MODULES=y"
[ "$GATE_TLS" = y ] && NATIVE_CFG="$NATIVE_CFG CONFIG_TLS=y"

die(){ echo "FAIL: $*" >&2; exit 1; }
have(){ command -v "$1" >/dev/null 2>&1; }
need_file(){ [ -f "$1" ] || die "no such file: $1"; }

_log_progress(){
  echo "$(date +%H:%M:%S) $*" >> "$PROGRESS_LOG" 2>/dev/null || true
}

# Auto-detect ccache prefix for 5-10x compiler acceleration across clean rebuilds
CCACHE_PREFIX=""
if [ "$USE_CCACHE" != "0" ] && have ccache; then
  if have clang; then CCACHE_PREFIX="CC=\"ccache clang\" "
  elif have gcc; then CCACHE_PREFIX="CC=\"ccache gcc\" "
  fi
fi

# The sanitizer runtime options belong to the config, decided once here so no
# stage can run ASan without detect_leaks=0 by forgetting to say so.
_sanenv(){
  case "$1" in
    *CONFIG_ASAN*)  echo "ASAN_OPTIONS=detect_leaks=0 PEN_BUDGET_MS=8000" ;;
    *CONFIG_UBSAN*) echo "UBSAN_OPTIONS=halt_on_error=1 PEN_BUDGET_MS=8000" ;;
    *CONFIG_TSAN*)  echo "TSAN_OPTIONS=halt_on_error=1 PEN_BUDGET_MS=8000" ;;
  esac
}

# ---------------------------------------------------------------- isolation --
# Every config writes the SAME ./dynajs, ./dynajsc and ./libdynajs.a, so two
# configs cannot share a tree: a TSan libdynajs.a got linked into a default
# build here, ld reporting undefined ___tsan_write8. Hence one tree per config.
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

# _tree NAME: a pristine build tree at $TREES/NAME. test262/ is 265 MB and
# the t262 stage reads only the main-tree copy, but api-inventory's standard
# name classification reads it too: a clone WITHOUT it marks every ES builtin
# "non-standard" and check-types reports 214 phantom drifts. Symlink it in
# (read-only usage) rather than copying 265 MB per tree. .obj/ and .git/ are
# rebuilt or unused. Copy on write, so a tree costs ~0.35 s of metadata
# rather than 69 MB of copying.
_tree(){
  local d="$TREES/$1" e b
  # "." means "run in the main tree, no clone" -- for stages whose targets
  # docker-mount $(CURDIR): a $TREES clone lives under /tmp, which Docker on
  # macOS cannot see, so the mount arrives empty and every cc fails.
  [ "$1" = . ] && return 0
  rm -rf "$d" && mkdir -p "$d" || return 1
  for e in "$ROOT"/* "$ROOT"/.[!.]*; do
    [ -e "$e" ] || continue
    b=${e##*/}
    case "$b" in test262|.obj|.git) continue ;; esac
    $CLONE "$e" "$d/" || return 1
  done
  ln -s "$ROOT/test262" "$d/test262" || return 1
  # The clone carries the last build's ./dynajs and ./libdynajs.a. Ask the
  # Makefile what an output is rather than listing them here, where they rot.
  ( cd "$d" && make clean >/dev/null 2>&1 )
}

# ------------------------------------------------------------- process pool --
# Each job owns a log, so concurrent stages cannot interleave and a failure
# still shows what it printed. Status travels through a file: `wait` reaps, and
# a reaped pid's status is no longer readable with `wait $pid`.
_P_NAMES=(); _P_LOGS=(); RUNDIR=

_rundir(){ [ -n "$RUNDIR" ] || RUNDIR=$(mktemp -d "${TMPDIR:-/tmp}/devsh.XXXXXX"); }

# A hung stage in a pool is invisible -- the pool simply never drains -- so
# every one is bounded. Wall clock: a stage blocked on a socket burns no CPU.
_bound(){
  if   [ "$TIMEOUT" -le 0 ] 2>/dev/null; then "$@"
  elif have timeout;  then timeout  -k 5 "$TIMEOUT" "$@"
  elif have gtimeout; then gtimeout -k 5 "$TIMEOUT" "$@"
  else "$@"; fi
}

_pstart(){
  local name="$1" log; shift
  _rundir
  log="$RUNDIR/${name//[^A-Za-z0-9._-]/_}.log"
  _P_NAMES+=("$name"); _P_LOGS+=("$log")
  _log_progress "[stage: $name] starting"
  if [ "$SERIAL" = 1 ]; then
    _bound "$@" >"$log" 2>&1; echo $? >"$log.rc"; return 0
  fi
  while [ "$(jobs -rp | wc -l)" -ge "$PAR" ]; do sleep 0.2; done
  { _bound "$@" >"$log" 2>&1; echo $? >"$log.rc"; } &
}

_preport(){   # $1=name $2=log
  local rc last errs; rc=$(cat "$2.rc" 2>/dev/null || echo 99)
  if [ "$rc" = 0 ]; then
    _log_progress "[stage: $1] ok"
    # Prefer the stage's own summary when it is speaking about itself: test262's
    # baseline count is the number the gate exists to check, and "ok" hides it.
    last=$(grep -vE '^[[:space:]]*$' "$2" 2>/dev/null | tail -1)
    case "$last" in "$1: "*) echo "$last" ;; *) echo "$1: ok" ;; esac
    return 0
  fi
  _log_progress "[stage: $1] FAILED (rc=$rc)"
  if [ "$rc" = 124 ]; then echo "FAIL: $1 TIMED OUT after ${TIMEOUT}s"
  else echo "FAIL: $1 (rc=$rc)"; fi
  errs=$(grep -iE "error:" "$2" 2>/dev/null | head -10 || true)
  if [ -n "$errs" ]; then
    echo "  Errors found:"
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
  for i in "${!_P_NAMES[@]}"; do
    _preport "${_P_NAMES[$i]}" "${_P_LOGS[$i]}" || bad=1
  done
  _P_NAMES=(); _P_LOGS=()
  return "$bad"
}

# ------------------------------------------------------- test fan-out --
# A test must run alone if it talks to a fixed external port (a database) or
# shares a fixed /tmp path with another test -- both are machine-global, so a
# peer would delete its fixtures. Decided from the sources, so it cannot rot.
# \< \> and NOT \b: \b is not a word boundary in POSIX ERE and matches nothing,
# which here would read exactly like "no test touches a database".
_SOLO_RE='\<(PostgreSQL|Redis|SQLite)\>'
_solo_set(){   # $1... = test files; prints the ones that must not run beside a peer
  local dups f
  dups=$(grep -ohE '/tmp/[A-Za-z0-9_.-]+' "$@" 2>/dev/null | sort | uniq -d)
  for f in "$@"; do
    if grep -qE "$_SOLO_RE" "$f" 2>/dev/null; then echo "$f"; continue; fi
    if [ -n "$dups" ] && grep -qF -- "$dups" "$f" 2>/dev/null; then echo "$f"; fi
  done
}

# _fan FILE...: run ./dynajs over each, concurrently, solo ones last and alone.
# Prints the count -- a run whose count varies is silently skipping cases.
_fan(){
  local d n=0 fail=0 t solo rc cmd f
  d=$(mktemp -d) || return 1
  solo=$(_solo_set "$@")
  for t in "$@"; do
    printf '%s\n' "$solo" | grep -qx "$t" && continue
    if [ "$SERIAL" = 1 ]; then
      n=$((n+1)); ./dynajs "$t" </dev/null >"$d/$n.out" 2>&1; echo "$? $t" >"$d/$n.rc"; continue
    fi
    while [ "$(jobs -rp | wc -l)" -ge "$JOBS" ]; do sleep 0.05; done
    n=$((n+1))
    # </dev/null: a spawned test inherits this shell's stdin otherwise.
    { ./dynajs "$t" </dev/null >"$d/$n.out" 2>&1; echo "$? $t" >"$d/$n.rc"; } &
  done
  wait
  for t in $solo; do
    n=$((n+1)); ./dynajs "$t" </dev/null >"$d/$n.out" 2>&1; echo "$? $t" >"$d/$n.rc"
  done
  for f in "$d"/*.rc; do
    [ -e "$f" ] || continue
    read -r rc cmd < "$f"
    [ "$rc" = 0 ] && continue
    fail=$((fail+1)); echo "FAIL($rc): $cmd"; tail -20 "${f%.rc}.out" 2>/dev/null | sed 's/^/    /'
  done
  rm -rf "$d"
  echo "  ran $n, failed $fail ($(printf '%s\n' "$solo" | grep -c . ) solo)"
  [ "$fail" = 0 ]
}

# `make test-native` is one recipe of 83 independent runs, so make cannot spread
# it. The list comes from `make -n` -- make's own expansion of $(NATIVE_TESTS),
# so nothing is copied here to drift -- and the rest of the target goes back to
# make. A recipe that is not this shape runs unchanged rather than be guessed at.
_native_run(){   # $@ = make config args
  local recipe list
  recipe=$(make -n "$@" test-native 2>/dev/null)
  list=$(printf '%s\n' "$recipe" | tr ' \t' '\n\n' | grep -E '^tests/[A-Za-z0-9_.-]+\.js$' | sort -u)
  if [ -z "$list" ] || ! printf '%s\n' "$recipe" | grep -q 'for t in tests/' \
     || ! printf '%s\n' "$recipe" | grep -q 'test-examples'; then
    echo "note: test-native recipe is not the expected shape, running it serially"
    make "$@" test-native; return $?
  fi
  # the recipe's own precondition; without it a missing native build reads as
  # 83 identical failures instead of one sentence.
  ./dynajs -e 'import("dyna:mathx")' >/dev/null 2>&1 || {
    echo "FAIL: ./dynajs cannot load dyna:* modules (needs CONFIG_NATIVE_MODULES=y)"; return 1; }
  _fan $list || return 1
  make "$@" test-examples
}

# ------------------------------------------------------------------ builds --
# _compile "<make args>": fail on any compiler error or (non-pre-existing)
# warning. Silent on success. Assumes this tree already holds this config.
_compile(){
  # NB: never pre-create .obj here -- that satisfies the Makefile's $(OBJDIR)
  # order-only prereq and skips its `mkdir .obj/examples .obj/tests`, racing a
  # parallel build after a clean. Let make own the dir; write the stamp after.
  local cfg="$*" log warn errs
  log=$(mktemp)
  if [ -n "$CCACHE_PREFIX" ] && [[ "$cfg" != *"CC="* ]]; then
    cfg="$CCACHE_PREFIX$cfg"
  fi
  if ! make -j"$BUILD_J" $cfg >"$log" 2>&1; then
    echo "FAIL: build [$cfg]"
    errs=$(grep -iE "error:" "$log" | head -12 || true)
    if [ -n "$errs" ]; then
      echo "  Compiler errors:"
      echo "$errs" | sed 's/^/    /'
    fi
    echo "  Tail of build log:"
    tail -20 "$log" 2>/dev/null | sed 's/^/    /'
    rm -f "$log"
    return 1
  fi
  # Cosmetic ld64 noise from the vendored openlibm archive's own symbol-table
  # metadata (CLAUDE.md "Deterministic libm"). Filtered NARROWLY -- only lines
  # naming libopenlibm.a -- so a real warning in our code still fails the gate.
  warn=$(grep -iE "warning:" "$log" | grep -v "loop not vectorized" \
         | grep -v "libopenlibm\.a.*malformed LC_DYSYMTAB" || true)
  rm -f "$log"
  [ -z "$warn" ] || { echo "FAIL: build warnings [$cfg]"; echo "$warn" | head -8; return 1; }
}

# _build: _compile in THIS tree, cleaning first iff the config changed. The
# clean is load-bearing -- the root outputs above are shared by every config --
# and is exactly why concurrent configs get a tree each instead of this one.
_build(){
  local cfg="$*"
  [ "$(cat "$STAMP" 2>/dev/null || true)" = "$cfg" ] || make clean >/dev/null 2>&1 || true
  _compile "$cfg" || return 1
  echo "$cfg" >"$STAMP" 2>/dev/null || true
}

# run a JS file or the `test` target under an optional sanitizer env
_run_target(){   # $1=target(file|test)  rest=args
  local t="$1"; shift || true
  if [ "$t" = "test" ]; then
    make test >/dev/null 2>&1 || die "make test"
    echo "make test: ok"
  else
    need_file "$t"
    ./dynajs "$t" "$@" || die "run $t"
  fi
}

_t262_full(){
  [ -f "$CONF" ] || die "$CONF missing (run: make test2-bootstrap)"
  local got
  # run-test262 threads on its own at cpu_count()-1; -T only so it honours the
  # budget when the gate is already running stages beside it.
  got=$(./run-test262 -c "$CONF" -a -T "$JOBS" 2>&1 | grep -oE '[0-9]+/[0-9]+ errors' | grep -oE '[0-9]+/[0-9]+')
  [ -n "$got" ] || die "test262 produced no Result line"
  # The ratchet is ONE-WAY: fewer failures than the pin is an improvement and
  # must not fail the gate; more is a regression. A grown corpus (denominator
  # up) at or under the pin still passes -- new tests are not new bugs.
  # Hand-lower the pin after an improvement lands.
  got_f=${got%/*}; got_t=${got#*/}
  base_f=${BASELINE%/*}; base_t=${BASELINE#*/}
  if [ "$got_f" -gt "$base_f" ]; then
    echo "FAIL: test262 regressed: got $got, pin allows $base_f failures"; exit 1
  elif [ "$got_t" -lt "$base_t" ]; then
    echo "FAIL: test262 corpus shrank: got $got, want at least */$base_t"; exit 1
  elif [ "$got" = "$BASELINE" ]; then echo "test262: $got ok"
  else echo "test262: $got ok (IMPROVED past the $BASELINE pin -- lower it)"; fi
}

# ------------------------------------------------------------- gate stages --
# A stage is a separate process, reached through `__stage`, because `timeout`
# runs a command and cannot run a shell function. $1 is the tree ("." for this
# one), $2 the config, $3 the runner. Building twice is a no-op make, so a
# phase-2 stage re-entering its tree costs nothing and proves the binary matches.
_stage(){
  local name="$1" cfg="$2" kind="$3" t env_kv; shift 3
  case "$kind" in
    pool|fuzz|ctest) [ "$name" = . ] || cd "$TREES/$name" || return 1 ;;
    *) if [ "$name" = . ]; then _build "$cfg" || return 1
       else cd "$TREES/$name" && _compile "$cfg" || return 1; fi ;;
  esac
  env_kv=$(_sanenv "$cfg")
  case "$kind" in
    build)  return 0 ;;
    pool)   make test-pool ;;
    # The round-2/3/4/5 C regressions. Like pool, each target compiles its own
    # sanitized (or sed-extracted) binary straight from sources, so the stage
    # needs no config build in its tree. The two uring targets SKIP loudly on a
    # docker-less host -- execute-or-SKIP, never a host-gate failure.
    # ac-caps/ds-core/aio-disk were CLAIMED folded here in round 5 but the
    # line never carried them (gate-orphans twice over); they are now.
    ctest)  make test-ds-btree-oom test-ac-caps test-aio-sendfile-busy \
              test-aio-connect-offload test-dns-foreign-answers test-exec-spawn \
              test-ds-core test-aio-disk test-uring-sendfile \
              test-uring-connect-offload test-uring-tick \
              test-bc-read-safety ;;
    # The fuzz targets have their own object set and their own hand-written link
    # lines, so a new call from an engine source breaks them and nothing else
    # notices -- lre_exec gaining a simd_init call did exactly that.
    # fuzz-audit first: libfuzzer is itself a hand-kept list, and it had gone
    # stale by three targets, so linking it proved less than it appeared to.
    fuzz)   make fuzz-audit && make -j"$BUILD_J" libfuzzer >/dev/null ;;
    smoke)  for t in "$@"; do env $env_kv ./dynajs "$t" </dev/null || return 1; done ;;
    native) [ -z "$env_kv" ] || export $env_kv
            _native_run $cfg </dev/null || return 1
            # test-api (8 value suites) and test-security (10 pen suites) ran
            # only under `make prepush`; the gate never saw them (audit 13.6.2).
            # Both need a native build, so they ride this stage under BOTH
            # sanitizers. Unquoted $cfg splits into the CONFIG_* args.
            make $cfg test-api || return 1
            make $cfg test-security || return 1 ;;
    tsan)   for t in "$@"; do env $env_kv ./dynajs "$t" </dev/null >/dev/null || return 1; done ;;
    tls)    [ -z "$env_kv" ] || export $env_kv
            make $cfg test-x509 test-tls-conn test-tls test-crypto-aead \
              || return 1 ;;
    *)      echo "FAIL: unknown stage kind $kind"; return 1 ;;
  esac
}

# _queue LABEL TREE CFG KIND [args...]: clone the tree and queue the stage.
# Cloning happens here, before anything builds into the tree being cloned.
_queue(){
  local label="$1" name="$2"; shift 2
  if [ "$SERIAL" = 1 ]; then _pstart "$label" bash "$0" __stage . "$@"; return; fi
  _tree "$name" || die "could not clone a build tree for $name"
  _pstart "$label" bash "$0" __stage "$name" "$@"
}

# _serial LABEL TREE CFG KIND [args...]: run one stage now, in the foreground.
_serial(){
  local label="$1" name="$2" log; shift 2
  _rundir; log="$RUNDIR/${label//[^A-Za-z0-9._-]/_}.log"
  [ "$SERIAL" = 1 ] && name=.
  _bound bash "$0" __stage "$name" "$@" >"$log" 2>&1; echo $? >"$log.rc"
  _preport "$label" "$log"
}

cmd="${1:-}"; shift 2>/dev/null || true
case "$cmd" in
  build)    _build "$@" || exit 1; echo "build: ok" ;;

  run)      [ $# -ge 1 ] || die "usage: run FILE [args]"; _build "" || exit 1; _run_target "$@" ;;

  test)     _build "" || exit 1; make test || die "make test" ;;

  asan)     [ $# -ge 1 ] || die "usage: asan FILE|test"
            _build "CONFIG_ASAN=y" || exit 1
            ASAN_OPTIONS=detect_leaks=0 _run_target "$@"; echo "asan: ok" ;;

  ubsan)    [ $# -ge 1 ] || die "usage: ubsan FILE|test"
            _build "CONFIG_UBSAN=y" || exit 1
            UBSAN_OPTIONS=halt_on_error=1 _run_target "$@"; echo "ubsan: ok" ;;

  openlibm) [ $# -ge 1 ] || die "usage: openlibm FILE|test"
            [ -f third_party/openlibm/libopenlibm.a ] || \
              die "third_party/openlibm/libopenlibm.a missing (clone+make it; see Makefile)"
            _build "CONFIG_OPENLIBM=y" || exit 1; _run_target "$@"; echo "openlibm: ok" ;;

  status)   echo "=== DynaJS Environment & Toolchain Status ==="
            echo "  Platform:     $(uname -s) $(uname -m)"
            echo "  CPU Cores:    $NCPU (jobs: $JOBS, build_j: $BUILD_J)"
            comp="none"
            if have clang; then comp="clang $(clang --version 2>/dev/null | head -1 | grep -oE '[0-9]+(\.[0-9]+)+' | head -1)";
            elif have gcc; then comp="gcc $(gcc --version 2>/dev/null | head -1 | grep -oE '[0-9]+(\.[0-9]+)+' | head -1)"; fi
            echo "  Compiler:     $comp"
            echo "  ccache:       $(have ccache && [ "$USE_CCACHE" != "0" ] && echo "enabled ($(ccache --version 2>/dev/null | head -1))" || echo "not enabled/found")"
            echo "  OpenSSL:      $(pkg-config --modversion openssl 2>/dev/null || echo "system/none")"
            echo "  SQLite:       $(pkg-config --modversion sqlite3 2>/dev/null || echo "not found")"
            echo "  libzstd:      $(pkg-config --modversion libzstd 2>/dev/null || echo "not found")"
            echo "  Current CFG:  $(cat "$STAMP" 2>/dev/null || echo "clean/unbuilt")"
            if [ -x ./dynajs ]; then
              echo "  Binary:       ./dynajs ($(ls -lh ./dynajs | awk '{print $5}'))"
              echo "  Native Std:   $(./dynajs -e 'import("dyna:mathx").then(()=>print("available")).catch(()=>print("no"))' 2>/dev/null || echo "no")"
            else
              echo "  Binary:       not built"
            fi
            echo "status: ok" ;;

  check)    echo "=== Running Static Integrity Checks ==="
            ./tools/core-purity.sh || die "core-purity"
            ./tools/doc-lint.sh || die "doc-lint"
            if [ -f bench/codegraph.py ]; then
              python3 bench/codegraph.py . 10 >/dev/null || die "codegraph"
              echo "codegraph: ok"
            fi
            echo "check: ok" ;;

  # Single-module iteration WITHOUT a full build: test-mods MODS=... links a
  # slim binary holding ONLY the named modules (+ their fixed companions) and
  # runs just those modules' tests. Needs a CONFIG_NATIVE_MODULES=y build on
  # disk; test targets skip the config-stamp wipe, so the main binary is safe.
  mod)      [ $# -ge 1 ] || die "usage: mod MODS  (e.g. mod csv,net -- see 'make test-mods')"
            make CONFIG_NATIVE_MODULES=y CONFIG_TLS=y test-mods MODS="$*" \
              || die "test-mods MODS=$*" ;;

  # Seconds-cost compile check of ONE translation unit (or all modified
  # src/*.c with no argument) -- the inner loop before any build at all.
  ccheck)   if [ $# -ge 1 ]; then make ccheck FILE="$1" || die "ccheck $1";
            else make ccheck || die "ccheck"; fi ;;

  # Concurrent-agent workspaces: full tree copies in temp dirs, patch-based
  # merge back (tools/ws.sh is the implementation and the documentation).
  ws)       [ $# -ge 1 ] || die "usage: ws create|list|diff|merge|reset|drop|shell ...";
            ./tools/ws.sh "$@" ;;

  quick)    _build "CONFIG_NATIVE_MODULES=y" || exit 1
            echo "running core sanity test suite..."
            ./dynajs --std tests/test_builtin.js || die "test_builtin"
            ./dynajs tests/test_language.js || die "test_language"
            ./dynajs tests/test_encoding.js || die "test_encoding"
            ./dynajs tests/test_json.js || die "test_json"
            ./dynajs tests/test_crypto.js || die "test_crypto"
            if [ $# -gt 0 ]; then
              for t in "$@"; do need_file "$t"; ./dynajs "$t" || die "run $t"; done
            fi
            echo "quick: ok" ;;

  bench)    [ $# -ge 1 ] || die "usage: bench FILE [args]"
            _build "CONFIG_NATIVE=y" || exit 1; need_file "$1"; ./dynajs "$@" || die "bench" ;;

  t262)     _build "" || exit 1
            if [ $# -ge 1 ]; then
              [ -f "$CONF" ] || die "$CONF missing (run: make test2-bootstrap)"
              # -d overrides the conf's testdir, so a bare subtree name resolves
              # to nothing and run-test262 answers with its help text.
              d="$1"; [ -d "$d" ] || d="test262/test/$1"
              [ -d "$d" ] || die "no such test262 subtree: $1"
              ./run-test262 -c "$CONF" -a -T "$JOBS" -d "$d" 2>&1 | grep -E "^Result:" || die "test262 subtree"
            else _t262_full; fi ;;

  rss)      [ $# -ge 1 ] || die "usage: rss FILE [N...]"; f="$1"; shift
            need_file "$f"; _build "" || exit 1
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

  amd64)    have docker || die "docker not installed"
            bash docker/build-and-test.sh amd64 || die "docker amd64 build"
            echo "amd64: ok" ;;

  # Two phases. The builds are independent proofs over the same sources and
  # share nothing once each has a tree, so they all run at once. The JS test
  # runs do NOT: eight of the native tests use a fixed /tmp path, so two
  # test-native runs would delete each other's fixtures. Those stay in order.
  gate)     BUILD_J="$GATE_BUILD_J"
            _clone_probe || { SERIAL=1
              echo "note: no copy-on-write clone here, running the gate serially"; }
            [ "$SERIAL" = 1 ] || echo "gate: $PAR stages at a time, make -j$BUILD_J each"

            # ---- phase 1: everything that only compiles, plus the lints ----
            _pstart "core-purity"  ./tools/core-purity.sh
            _pstart "doc-lint"     ./tools/doc-lint.sh
            _queue  "tsan (pool)"  pool "" pool
            # "." not a CoW tree: the uring targets docker-mount $(CURDIR),
            # and Docker on macOS cannot see /tmp (where $TREES lives) -- the
            # mount arrives empty and every cc dies on its first source file.
            # The stage needs no isolation anyway: every target compiles its
            # own standalone binary into .obj.
            _queue  "C regression tests"  . "" ctest
            _queue  "build: asan"  asan  "CONFIG_ASAN=y"  build
            _queue  "build: ubsan" ubsan "CONFIG_UBSAN=y" build
            # The NATIVE surface -- dyna-aio, dyna-evloop, the whole dyna:net
            # stack, http, structures, dataframe, ml -- is behind
            # CONFIG_NATIVE_MODULES, so a default build compiles NONE of it: 37
            # objects, not one of them a module. When OpenSSL >= 3.0 is present
            # the native builds also enable CONFIG_TLS, or the TLS stack, HTTPS
            # and the AEAD ciphers are compiled by no gate at all.
            _queue  "build: asan (native modules)"  asan-nat \
                    "CONFIG_ASAN=y ${NATIVE_CFG}"  build
            _queue  "build: ubsan (native modules)" ubsan-nat \
                    "CONFIG_UBSAN=y ${NATIVE_CFG}" build
            _queue  "build: tsan (native modules)"  tsan \
                    "CONFIG_TSAN=y ${NATIVE_CFG}" build
            _queue  "fuzz targets link" fuzz "" fuzz
            _build "" || die "build"; echo "build: ok"
            _pwait || exit 1

            # ---- phase 2: the runs. test262 reads only test262/ and this
            # tree's ./run-test262, so it overlaps; the runs themselves do not.
            _pstart "test262" bash "$0" __t262

            rc=0
            if [ $# -gt 0 ]; then
              for t in "$@"; do ./dynajs "$t" || die "smoke $t"; done
              echo "smoke: ok"
              _serial "asan"  asan  "CONFIG_ASAN=y"  smoke "$@" || rc=1
              _serial "ubsan" ubsan "CONFIG_UBSAN=y" smoke "$@" || rc=1
            fi
            make test >/dev/null 2>&1 || die "make test"; echo "make test: ok"
            _serial "asan (native modules)"  asan-nat \
                    "${NATIVE_CFG}"  native || rc=1
            _serial "ubsan (native modules)" ubsan-nat \
                    "${NATIVE_CFG}" native || rc=1
            # ThreadSanitizer over the code that actually spawns threads. The
            # HTTP acceptor/worker path has three pthread_create sites and was
            # under TSan in NO target. Proved live by injecting an
            # unsynchronised counter into dyn_http_worker_main.
            _serial "tsan (threaded http)" tsan \
                    "CONFIG_TSAN=y ${NATIVE_CFG}" tsan \
                    tests/test_http.js tests/test_http_keepalive.js || rc=1
            # The TLS-requiring tests are NOT in NATIVE_TESTS (that build has
            # no TLS), so they run here only when OpenSSL was detected. Without
            # them the TLS seam is proven by no gate.
            if [ "$GATE_TLS" = y ]; then
              _serial "tls (native, asan)" asan-nat \
                      "CONFIG_ASAN=y ${NATIVE_CFG}" tls || rc=1
              _serial "tls (native, ubsan)" ubsan-nat \
                      "CONFIG_UBSAN=y ${NATIVE_CFG}" tls || rc=1
            else
              echo "tls (native): SKIPPED -- no OpenSSL >= 3.0 (brew install openssl@3)"
            fi

            _pwait || rc=1
            [ "$rc" = 0 ] || exit 1
            echo "gate: ok" ;;

  clean)    make clean >/dev/null 2>&1; rm -rf "$TREES" "$ROOT/.dev"; rm -f "$STAMP"; echo "clean: ok" ;;

  # internal: one gate stage / the test262 pass, each as its own process so
  # `timeout` can bound it. Not part of the command list.
  __stage)  _stage "$@"; exit $? ;;
  __t262)   _t262_full; exit $? ;;

  ""|-h|--help|help)
            sed -n '2,/^set -uo/{/^set -uo/d;p;}' "$0" ;;
  *)        die "unknown command: $cmd (try ./dev.sh help)" ;;
esac
