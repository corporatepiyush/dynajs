#!/usr/bin/env bash
# tools/prepush-parallel.sh -- the `make prepush` gate, scheduled by DEPENDENCY
# instead of habit. Same ten proofs as the serial recipe (kept verbatim in the
# Makefile as `prepush-serial`), grouped by what they actually contend for:
#
#   phase A   codegraph + import/orphan audit (parse-only, pure readers of
#             src/tests) run BESIDE the clean+build (which owns .obj). A
#             stage that parses sources cannot race a stage that writes
#             objects, so the lints ride in the build's shadow.
#   phase B   after the build: the suites on a PREPUSH_PAR worker pool, with
#             two scheduling rules measured into existence (ws-build session):
#             - the fuzz link proof runs EXCLUSIVELY first: it is the only
#               pool candidate that writes .obj at scale (libdynajs.fuzz.a,
#               fuzz_* binaries), and concurrent with `make test`'s bjson
#               compile it died with "Bad file descriptor" under fd pressure;
#             - test-native holds the full core budget (it is the critical
#               path: ~240s of 83 suites) while api/security/repl/tls are
#               single-digit-second stragglers; coretest and security keep
#               the NCPU/PAR fan cap so concurrent suites cannot pile up.
#             Contention was checked, not assumed: the pure-run stages share
#             no test file and no /tmp literal (scanned: core x api x
#             security = 0 shared files, 0 tmp overlap), so their fixtures
#             cannot collide.
#
# Fail-closed like the hook: any non-zero stage (or a red lint in phase A)
# fails the gate; every stage is bounded by PREPUSH_TIMEOUT and logs to
# .prepush/<stage>.log (NOT .obj/: `make clean` wipes it mid-gate).
#
# Knobs: PREPUSH_J (build -j), PREPUSH_PAR (phase-B concurrency, default 3),
#        PREPUSH_TIMEOUT (per-stage seconds, default 1800).
# One stage only (debug a failure without the full gate):
#        bash tools/prepush-parallel.sh __stage <name>   (e.g. repl, coretest)
#        -- suite stages need the tree the gate's build already produced.
set -u
cd "$(git rev-parse --show-toplevel 2>/dev/null || echo .)" || exit 2

# This script normally runs AS A RECIPE LINE of `make prepush`, which inherits
# the outer make's MAKEFLAGS -- including --jobserver-auth fds that are only
# valid for a child reached through $(MAKE). Our inner makes then die with
# "N: Bad file descriptor" trying to read the jobserver pipe. Scrub it: every
# make below passes its own -j explicitly. (Measured into existence: the fuzz
# stage failed this way ONLY when invoked under `make prepush`.)
MAKEFLAGS=
export MAKEFLAGS

NCPU=$( (command -v nproc >/dev/null 2>&1 && nproc) || sysctl -n hw.ncpu 2>/dev/null || echo 4 )
PREPUSH_J=${PREPUSH_J:-$NCPU}
PAR=${PREPUSH_PAR:-3}
TMO=${PREPUSH_TIMEOUT:-1800}
LOGDIR=.prepush
mkdir -p "$LOGDIR" || exit 2

have(){ command -v "$1" >/dev/null 2>&1; }
TIMEOUT_BIN=$(have timeout && echo timeout || { have gtimeout && echo gtimeout || true; })
bound(){ if [ -n "$TIMEOUT_BIN" ]; then "$TIMEOUT_BIN" -k 5 "$TMO" "$@"; else "$@"; fi; }

# OpenSSL >= 3.0 detection, identical to the Makefile's PREPUSH_TLS block so
# the gate's TLS decision cannot drift from the build's.
PREPUSH_OPENSSL_PC="$(brew --prefix openssl@3 2>/dev/null || brew --prefix openssl 2>/dev/null)/lib/pkgconfig"
PREPUSH_TLS=$(PKG_CONFIG_PATH="$PREPUSH_OPENSSL_PC:${PKG_CONFIG_PATH:-}" \
  pkg-config --exists 'openssl >= 3.0' 2>/dev/null && echo y)
NATIVE_CFG="CONFIG_NATIVE_MODULES=y"
[ "$PREPUSH_TLS" = y ] && NATIVE_CFG="$NATIVE_CFG CONFIG_TLS=y"
# MULTI_TESTS override, passed as a PLAIN environment variable (PREPUSH_MULTI_TESTS):
# command-line variable overrides from the caller ride MAKEFLAGS, which this
# script must scrub (jobserver fds, see above) -- an env var survives that.
PREPUSH_MULTI_TESTS=${PREPUSH_MULTI_TESTS:-}

say(){ echo "$*"; }
fail_stage(){   # NAME RC
  say "FAIL: $1 (rc=$2) -- log: $LOGDIR/$1.log"
  [ "$2" = 124 ] && say "       (stage was KILLED: exceeded PREPUSH_TIMEOUT=${TMO}s; raise it: PREPUSH_TIMEOUT=3600 make prepush)"
  tail -25 "$LOGDIR/$1.log" 2>/dev/null | sed 's/^/    /'
  say "       re-run just this stage:  bash $0 __stage $1"
}
# per-suite fan cap for the stages that fan out internally (run-tests-parallel)
fan_cap(){ local c=$(( NCPU / PAR )); [ "$c" -lt 1 ] && c=1; echo "$c"; }

# ------------------------------------------------------------- stage bodies --
# Each is the serial recipe's command, VERBATIM, behind a name so `timeout`
# can bound it as a process.
stage_codegraph(){
  python3 bench/codegraph.py . --report > bench/codegraph_report.txt 2>&1 || return 1
  local sum nf nfn
  sum=$(grep -E '^codegraph-[a-z0-9]+: [0-9]+ files parsed' bench/codegraph_report.txt | head -1)
  nf=$(printf '%s' "$sum" | sed -E 's/^codegraph-[a-z0-9]+: ([0-9]+) files.*/\1/')
  nfn=$(grep -E '^ *[0-9]+ symbols parsed' bench/codegraph_report.txt | head -1 | sed -E 's/ *([0-9]+) symbols.*/\1/')
  case "$nf$nfn" in ''|*[!0-9]*) nf=0; nfn=0;; esac
  [ "$nf" -ge 1 ] && [ "$nfn" -ge 1 ] || { echo "codegraph parsed $nf files / $nfn symbols -- wrong root or moved tree."; return 1; }
  echo "$sum ($nfn symbols)"
}
stage_defects(){
  # P10: the defect scanner gate. The selftest proves every check still
  # detects its class; the baseline pins the repo's accepted findings.
  python3 tools/test_codegraph_defects.py >/dev/null 2>&1 \
    || { echo "defect selftest FAILED (a check lost its plant)"; return 1; }
  python3 bench/codegraph.py . --defects --defects-baseline tools/defects-baseline.json >/dev/null 2>&1 \
    || { echo "defect gate: NEW findings vs baseline (or ERROR-severity present); run: python3 bench/codegraph.py . --defects --defects-all"; return 1; }
  echo "defect selftest + baseline gate clean"
}
stage_imports(){
  python3 tools/check-unused-imports.py tests || return 1
  python3 tools/check-orphan-tests.py || return 1
  echo "imports & orphan tests clean"
}
stage_build(){
  ${MAKE:-make} --no-print-directory clean >/dev/null || return 1
  ${MAKE:-make} -j"$PREPUSH_J" --no-print-directory $NATIVE_CFG || return 1
  ./dynajs -e 'import("dyna:mathx")' >/dev/null 2>&1 || {
    echo "prepush built a binary that cannot load dyna:*. Suites would SKIP and print green."; return 1; }
  echo "build ok (-j$PREPUSH_J, $NATIVE_CFG)"
}
stage_fuzz(){
  ${MAKE:-make} --no-print-directory PREPUSH_J=$PREPUSH_J prepush-fuzz
}
stage_coretest(){
  DEV_JOBS=$(fan_cap) ${MAKE:-make} --no-print-directory test
}
stage_tls(){
  ${MAKE:-make} --no-print-directory $NATIVE_CFG test-tls test-tls-conn test-x509 test-crypto-aead
}
stage_native(){
  # NB: MULTI_TESTS must be a COMMAND-LINE argument of the inner make. An env
  # var loses to the Makefile's own MULTI_TESTS= assignment, and a spaced
  # value in a command-prefix assignment gets word-split by bash (both
  # measured into existence). One quoted argv element = one assignment.
  if [ -n "$PREPUSH_MULTI_TESTS" ]; then
    DEV_JOBS=$NCPU ${MAKE:-make} --no-print-directory $NATIVE_CFG \
      "MULTI_TESTS=$PREPUSH_MULTI_TESTS" test-native
  else
    DEV_JOBS=$NCPU ${MAKE:-make} --no-print-directory $NATIVE_CFG test-native
  fi
}
stage_api(){
  ${MAKE:-make} --no-print-directory test-api
}
stage_security(){
  DEV_JOBS=$(fan_cap) ${MAKE:-make} --no-print-directory test-security
}
stage_repl(){
  ${MAKE:-make} --no-print-directory test-repl
}

# ------------------------------------------------------------- worker pool --
P_NAMES=(); P_IDX=()
STAGE_N=0
STAGE_TOTAL=11   # codegraph defects imports build fuzz | coretest native api security repl tls
pstart(){   # NAME: queue one bounded stage with its own log + rc files
  local name="$1"
  while [ "$(jobs -rp | wc -l)" -ge "$PAR" ]; do sleep 0.1; done
  P_NAMES+=("$name")
  STAGE_N=$((STAGE_N + 1)); P_IDX+=("$STAGE_N")
  say "  start [$STAGE_N/$STAGE_TOTAL] $name"
  {
    t0=$SECONDS
    bound bash "$0" __stage "$name" >"$LOGDIR/$name.log" 2>&1
    rc=$?
    echo "$rc" > "$LOGDIR/$name.rc"
    echo "$((SECONDS - t0))" > "$LOGDIR/$name.ok"
  } &
}
pwait(){
  wait
  local bad=0 i n rc
  for i in "${!P_NAMES[@]}"; do
    n=${P_NAMES[$i]}
    rc=$(cat "$LOGDIR/$n.rc" 2>/dev/null || echo 99)
    if [ "$rc" = 0 ]; then
      printf '       [%d/%d] %s ok in %ss\n' "${P_IDX[$i]}" "$STAGE_TOTAL" "$n" "$(cat "$LOGDIR/$n.ok" 2>/dev/null || echo '?')"
    else
      bad=1; fail_stage "$n" "$rc"
    fi
  done
  P_NAMES=(); P_IDX=()
  return "$bad"
}

case "${1:-}" in
  __stage)
    name="${2:-?}"
    case "$name" in
      codegraph|defects|imports|build|fuzz|coretest|tls|native|api|security|repl) ;;
      *) echo "unknown stage: $name"; exit 97 ;;
    esac
    "stage_$name"
    exit $?
    ;;
esac

# ------------------------------------------------------------------- driver --
t0=$SECONDS
say "================================================================="
say "prepush: $STAGE_TOTAL-stage proof gate, 2 waves (build -j$PREPUSH_J, $PAR workers/wave, TLS: $( [ "$PREPUSH_TLS" = y ] && echo yes || echo no))"
say "expect: several minutes end to end (the core suite alone is ~4 min;"
say "       per-stage cap ${TMO}s) -- logs land in $LOGDIR/<stage>.log"
say "================================================================="

# -- phase A: parse-only lints beside the build (which owns .obj) --
pstart codegraph
pstart defects
pstart imports
STAGE_N=$((STAGE_N + 1))
say "  start [$STAGE_N/$STAGE_TOTAL] build (clean + make -j$PREPUSH_J)"
stage_build; brc=$?
pwait; pa_rc=$?
if [ "$brc" != 0 ]; then fail_stage build "$brc"; say "       [build] FAIL"; exit 1; fi
say "       [$STAGE_N/$STAGE_TOTAL] build ok"
if [ "$pa_rc" != 0 ]; then say "prepush: FAIL (phase A lint)"; exit 1; fi

# -- phase B: fuzz link proof EXCLUSIVELY (sole .obj writer left), then the
#    suites on the worker pool --
STAGE_N=$((STAGE_N + 1))
say "  start [$STAGE_N/$STAGE_TOTAL] fuzz (audit + link proof)"
_t0=$SECONDS
if stage_fuzz; then
  say "       [$STAGE_N/$STAGE_TOTAL] fuzz ok in $((SECONDS - _t0))s"
else
  fail_stage fuzz "$?"; say "prepush: FAIL"; exit 1
fi
pstart coretest
pstart native
pstart api
pstart security
pstart repl
pstart tls
if ! pwait; then
  say "prepush: FAIL"
  exit 1
fi

say "================================================================="
say "prepush: OK -- all 10 stages passed in $((SECONDS - t0))s"
say "================================================================="
