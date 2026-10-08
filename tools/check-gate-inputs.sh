#!/usr/bin/env bash
# tools/check-gate-inputs.sh -- every path a GATE TABLE names must be a TRACKED
# file that exists on disk.
#
# B1-09: SECURITY_TESTS_TABLE named bench/_sec.js, which .gitignore excludes
# (`.gitignore: bench/*`). On the author's machine the security stage ran 11
# suites; on a clean checkout it died with "test file not found". Nothing in
# the gate could see the difference, because the runner only checks the
# filesystem, never the index.
#
# This checker closes that class: it derives the gate-table universe from
# build.sh (not from a hand-kept list, which would rot), and requires every
# entry to be
#   1. present on disk (the runner's own check), and
#   2. tracked by git (`git ls-files --error-unmatch`), and
#   3. not matched by `git check-ignore` (an ignore rule would strip it from
#      every clone even if it were staged once).
#
# Usage: tools/check-gate-inputs.sh [--quiet]   (run from the repo root)
set -uo pipefail
cd "$(dirname "$0")/.." || exit 2

QUIET=0
[ "${1:-}" = "--quiet" ] && QUIET=1

# The gate tables, read out of build.sh by NAME. Adding a table here is the
# only step needed to bring it under the tracked-file rule.
TABLES="CORE_TESTS_TABLE SECURITY_TESTS_TABLE API_TESTS_TABLE MT_MULTI_TABLE MT_MULTI MT_STANDALONE_TABLE"

command -v git >/dev/null 2>&1 || { echo "check-gate-inputs: SKIPPED -- git not available" >&2; exit 0; }
git rev-parse --git-dir >/dev/null 2>&1 || {
  echo "check-gate-inputs: SKIPPED -- not a git checkout (nothing to track against)" >&2; exit 0; }

# 1. the universe: every tests/... and bench/... path named by a gate table.
universe=$(for t in $TABLES; do
  sed -n "s/^${t}=\"\(.*\)\"[[:space:]]*$/\1/p" build.sh
done | tr ' ' '\n' | grep -E '^(tests|bench)/.*\.(js|sh|py)$' | sort -u)

# every tests/bench .js path named on a NON-COMMENT line of build.sh: those are
# the suites a recipe runs directly (./dynajs FILE, run-tests-parallel FILE, ...)
recipes=$(grep -vE '^[[:space:]]*#' build.sh | grep -oE '(tests|bench)/[A-Za-z0-9_./-]*\.js' | sort -u | tr '\n' ' ')
# ...plus the suites a referenced tests/*.sh DRIVER launches. build.sh names the
# driver, not the suite (test-base58-alloc -> tests/run_base58_alloc.sh -> the
# suite it execs), so without this a genuinely-run suite reads as an orphan.
for w in $(grep -vE '^[[:space:]]*#' build.sh | grep -oE 'tests/[A-Za-z0-9_.-]*\.sh' | sort -u); do
  [ -f "$w" ] || continue
  recipes="$recipes $(grep -oE 'tests/[A-Za-z0-9_.-]*\.js' "$w" 2>/dev/null | sort -u | tr '\n' ' ')"
done
recipes="$recipes $(printf '%s\n' tests/blackbox/*.js 2>/dev/null | tr '\n' ' ')"

if [ -z "$universe" ]; then
  echo "FAIL: check-gate-inputs: no gate-table path was derived from build.sh --" >&2
  echo "      the table names changed shape; this checker must follow them." >&2
  exit 1
fi

n=0; missing=""; untracked=""; ignored=""
for f in $universe; do
  n=$((n+1))
  [ -f "$f" ] || { missing="$missing $f"; continue; }
  git ls-files --error-unmatch -- "$f" >/dev/null 2>&1 \
    || { untracked="$untracked $f"; continue; }
  if git check-ignore -q -- "$f" 2>/dev/null; then
    ignored="$ignored $f"
  fi
done

rc=0
[ -n "$missing" ]   && { echo "FAIL: gate tables name files that do not exist:$missing" >&2; rc=1; }
[ -n "$untracked" ] && { echo "FAIL: gate tables name UNTRACKED files (a clean checkout cannot run them):$untracked" >&2
                         echo "      git add them, or point the table at a tracked path." >&2; rc=1; }
[ -n "$ignored" ]   && { echo "FAIL: gate tables name IGNORED files (stripped from every clone):$ignored" >&2
                         echo "      the .gitignore rule that hides them is the defect (see .gitignore)." >&2; rc=1; }

# ---------------------------------------------------------------------------
# 2. the REVERSE direction: every tests/*.js in the tree must be REFERENCED by
#    something a gate actually executes. B1-08's whole finding was suites that
#    existed, looked gated, and ran nowhere; the orphan audit that was supposed
#    to catch it (tools/check-orphan-tests.py) counts a suite as "run" when its
#    BASENAME appears anywhere in build.sh -- including inside an EXCLUSION list
#    (MT_STANDALONE_TABLE) and inside usage text -- so it cannot fail.
#
#    Reference set = the gate tables above + every tests/bench .js path named on
#    a NON-COMMENT line of build.sh (a recipe runs those directly) + the whole
#    tests/blackbox/ corpus (tests/blackbox/run.sh enumerates it by glob).
# SCOPE: tests/test_*.js + tests/blackbox/*.js. Deliberately NOT tests/bench_*.js,
# tests/rss_*.js, tests/oracle_*.js, tests/probe_*.js, fixtures and child
# drivers: those are benchmarks, leak probes, diagnostics and helpers, and
# demanding they be gate rows would be the same decoration B1-08 complained
# about. A REGRESSION suite (test_*) that nothing runs is a different thing.
#
# Exceptions are NOT a second list: KNOWN_UNRUN is read live out of
# tools/check-orphan-tests.py, so the two checks cannot disagree.
known_unrun=" $(sed -n "/^KNOWN_UNRUN = {/,/^}/p" tools/check-orphan-tests.py 2>/dev/null \
                | grep -oE "'tests/[A-Za-z0-9_.-]+\.js'" | tr -d "'" | tr '\n' ' ')"
# Suites whose ONLY runner is a cross-platform container leg. B1-17 established
# that the amd64/musl/io_uring docker legs are MANUAL BY CONSTRUCTION: they
# need a Linux container and a purpose-built image, so no host gate can execute
# them. Listing them here states that fact; it does not claim coverage.
# TICKET B1-17/docker-legs-are-manual -- when those legs become gate stages,
# delete the entry here and the guard starts requiring a table.
cross_platform_only="tests/test_http_keepalive.js tests/test_ml_oracle.js
tests/test_ml_sklearn_vectors.js tests/test_object_literal_presize.js
tests/test_regexp_prefilter.js"
n_ref=0; unreferenced=""
for f in $(ls tests/test_*.js tests/blackbox/*.js 2>/dev/null | sort -u); do
  n_ref=$((n_ref+1))
  base=$(basename "$f")
  case " $universe $recipes " in
    *" $f "*|*" $base "*) continue ;;
  esac
  case "$known_unrun" in *" $f "*) continue ;; esac
  case "
$cross_platform_only" in *"$f"*) continue ;; esac
  unreferenced="$unreferenced $f"
done
if [ -n "$unreferenced" ]; then
  echo "FAIL: these tests/*.js files are referenced by NO gate table and NO recipe:" >&2
  for f in $unreferenced; do echo "      $f" >&2; done
  echo "      Add each to the table that owns its module (mt_modtests, or one of" >&2
  echo "      CORE/SECURITY/API/MULTI/STANDALONE), or record why it is driven as a" >&2
  echo "      child of another suite in tools/check-orphan-tests.py KNOWN_UNRUN." >&2
  rc=1
fi

[ "$rc" = 0 ] || exit 1
[ "$QUIET" = 1 ] || \
  echo "  check-gate-inputs: $n gate-table paths (all tracked+present), $n_ref tests/*.js all referenced"
exit 0