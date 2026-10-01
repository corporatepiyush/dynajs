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

set -u

cd "$(dirname "$0")/../.." || exit 1
DYN="${1:-./dynajs}"
shift 2>/dev/null || true
FILTER="${*:-}"

if [ ! -x "$DYN" ]; then
    echo "no engine binary at $DYN (build first: make CONFIG_NATIVE_MODULES=y)" >&2
    exit 2
fi

pass=0
fail=0
skipped=0
failed_names=""

for f in tests/blackbox/bb_*.js; do
    [ -e "$f" ] || { echo "no bb_*.js suites found under tests/blackbox/" >&2; exit 2; }
    base=$(basename "$f" .js)
    case "$base" in *"$FILTER"*) ;; *) continue ;; esac

    flags=""
    case "$base" in
        bb_std_os) flags="--std" ;;
    esac

    out=$("$DYN" $flags "$f" 2>&1)
    rc=$?

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
    elif printf '%s\n' "$out" | grep -q "ALL-SKIP"; then
        skipped=$((skipped + 1))
        echo "SKIP $base (nothing runnable on this platform)"
    else
        fail=$((fail + 1))
        failed_names="$failed_names $base"
        echo "FAIL $base (exit $rc)"
        printf '%s\n' "$out" | tail -8 | sed 's/^/    | /'
    fi
done

echo
echo "blackbox: $pass passed, $fail failed, $skipped with platform skips"
if [ "$fail" -ne 0 ]; then
    echo "failed suites:$failed_names"
    exit 1
fi
exit 0
