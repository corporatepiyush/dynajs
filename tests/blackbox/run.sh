#!/bin/sh
# Black-box contract test runner.
#
# Runs every tests/blackbox/bb_*.js against the engine and reports a summary.
# Expectations in these files were derived from dynajs.d.ts (the contract)
# WITHOUT consulting engine sources or observing engine behavior — so a
# failure here is evidence of a contract violation (engine bug) or a
# mis-derived test; both are triaged in the fix phase.
#
# Usage:
#   tests/blackbox/run.sh [path-to-dynajs] [bb_globs...]
#   tests/blackbox/run.sh ./dynajs              # all suites
#   tests/blackbox/run.sh ./dynajs bb_bytes     # suites matching substring
#
# bb_std_os.js is run with --std (it tests the --std compat layer itself);
# every other suite must run with no flags.
#
# Honesty rules (B1-06.7):
#  - a suite is SKIPped only when it exited 0 AND said ALL-SKIP: a crash after
#    the marker used to be scored as a platform skip;
#  - every suite runs under an EXTERNAL wall bound (BB_TIMEOUT, default 180s),
#    so a wedged suite cannot park the stage;
#  - a filter that matches NO suite is an error (exit 2), not "0 passed";
#  - a suite that prints no "all tests passed" AND no ALL-SKIP is a FAILURE,
#    not a pass: silence is not a verdict.

set -u

cd "$(dirname "$0")/../.." || exit 1
DYN="${1:-./dynajs}"
shift 2>/dev/null || true
FILTER="${*:-}"
BB_TIMEOUT="${BB_TIMEOUT:-180}"
BOUNDED="$(pwd)/tools/bounded-run.sh"

if [ ! -x "$DYN" ]; then
    echo "no engine binary at $DYN (build first: ./build.sh build CONFIG_NATIVE_MODULES=y)" >&2
    exit 2
fi
if [ ! -x "$BOUNDED" ]; then
    echo "no $BOUNDED -- refusing to run the black-box suites UNBOUNDED" >&2
    exit 2
fi

pass=0
fail=0
skipped=0
ran=0
failed_names=""

for f in tests/blackbox/bb_*.js; do
    [ -e "$f" ] || { echo "no bb_*.js suites found under tests/blackbox/" >&2; exit 2; }
    base=$(basename "$f" .js)
    case "$base" in *"$FILTER"*) ;; *) continue ;; esac

    flags=""
    case "$base" in
        bb_std_os) flags="--std" ;;
    esac

    ran=$((ran+1))
    marker="$(pwd)/.obj/.bb-bounded.$$"
    rm -f "$marker"
    # shellcheck disable=SC2086
    BOUNDED_RUN_MARKER="$marker" "$BOUNDED" "$BB_TIMEOUT" "$(( BB_TIMEOUT / 2 ))" "bb_$base" -- \
        "$DYN" $flags "$f" >".obj/.bb-out.$$" 2>&1
    rc=$?
    out=$(cat ".obj/.bb-out.$$" 2>/dev/null)
    rm -f ".obj/.bb-out.$$"

    # A suite may legitimately print SKIP lines (platform-conditional
    # modules, e.g. dyna:uring off Linux) while still passing everything
    # it ran; the contract requires the pass line regardless.
    if [ "$rc" -eq 0 ] && printf '%s\n' "$out" | grep -q "all tests passed"; then
        if printf '%s\n' "$out" | grep -q "SKIP("; then
            skipped=$((skipped + 1))
            summary=$(printf '%s\n' "$out" | grep -E "SKIP|all tests passed" | tr '\n' ' ')
            echo "PASS(skip) $base  $summary"
        else
            pass=$((pass + 1))
            echo "PASS $base  [$(printf '%s\n' "$out" | grep 'all tests passed' | tail -1)]"
        fi
    elif [ "$rc" -eq 0 ] && printf '%s\n' "$out" | grep -q "ALL-SKIP"; then
        # a clean exit is part of the SKIP verdict: a crash after the marker
        # is a failure and must be reported as one
        skipped=$((skipped + 1))
        echo "SKIP $base (nothing runnable on this platform)"
    elif [ -f "$marker" ]; then
        fail=$((fail + 1))
        failed_names="$failed_names $base"
        echo "FAIL $base (TIMEOUT: exceeded the ${BB_TIMEOUT}s external bound)"
        printf '%s\n' "$out" | tail -8 | sed 's/^/    | /'
    else
        fail=$((fail + 1))
        failed_names="$failed_names $base"
        echo "FAIL $base (exit $rc)"
        printf '%s\n' "$out" | tail -8 | sed 's/^/    | /'
    fi
done
rm -f "$(pwd)/.obj/.bb-bounded.$$" 2>/dev/null

if [ "$ran" -eq 0 ]; then
    echo "blackbox: filter '$FILTER' matched NO suite -- that is not coverage" >&2
    echo "          (ls tests/blackbox | grep '$FILTER')" >&2
    exit 2
fi

echo
echo "blackbox: $pass passed, $fail failed, $skipped with platform skips"
if [ "$fail" -ne 0 ]; then
    echo "failed suites:$failed_names"
    exit 1
fi
exit 0
