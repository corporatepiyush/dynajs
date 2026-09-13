#!/bin/sh
# Run the whole suite AND every example, on whatever platform this is.
#
# Two bounds per case, because the two ways a case hangs need different
# instruments: a spin burns CPU (ulimit -t catches it), a blocked socket burns
# none (only wall clock catches it). A timeout is reported DISTINCTLY from a
# failure -- "it never finished" and "it returned the wrong answer" lead to
# different investigations.
#
# Usage: tools/xplat-full.sh [wall_seconds]
set -u
WALL=${1:-60}
CPU=$((WALL + 10))
OS=$(uname -s)-$(uname -m)
pass=0; fail=0; tmo=0; crash=0
FAILED=""; TIMEDOUT=""; CRASHED=""

have_timeout=0
command -v timeout >/dev/null 2>&1 && have_timeout=1

run_one() {                     # $1 = label, $2... = command
    label=$1; shift
    if [ $have_timeout = 1 ]; then
        ( ulimit -t $CPU 2>/dev/null; exec timeout -s KILL "$WALL" "$@" ) \
            >/tmp/xf.out 2>&1
    else
        ( ulimit -t $CPU 2>/dev/null; exec "$@" ) >/tmp/xf.out 2>&1
    fi
    rc=$?
    case $rc in
        0)   pass=$((pass+1));  printf "  ok    %s\n" "$label" ;;
        124|137) tmo=$((tmo+1)); TIMEDOUT="$TIMEDOUT $label"
             printf "  TMO   %s (>${WALL}s)\n" "$label" ;;
        # 128+n is a signal: a crash, not a wrong answer.
        13[0-9]|14[0-9]|15[0-9]|16[0-9])
             crash=$((crash+1)); CRASHED="$CRASHED $label"
             printf "  CRASH %s (signal %s)\n" "$label" "$((rc-128))"
             tail -3 /tmp/xf.out | sed 's/^/          /' ;;
        *)   fail=$((fail+1)); FAILED="$FAILED $label"
             printf "  FAIL  %s (exit %s)\n" "$label" "$rc"
             tail -3 /tmp/xf.out | sed 's/^/          /' ;;
    esac
}

echo "=== xplat-full on $OS (wall ${WALL}s, cpu ${CPU}s per case) ==="
[ -x ./dynajs ] || { echo "FAIL: no ./dynajs -- build first"; exit 1; }

echo "--- make test ---"
run_one "make-test" make CONFIG_NATIVE_MODULES=y test

echo "--- make test-native ---"
run_one "make-test-native" make CONFIG_NATIVE_MODULES=y test-native

echo "--- examples ---"
# CONFIG_SHARED_LIBS is unset on Darwin (Makefile: ifndef CONFIG_DARWIN), so
# .so modules are not built there and the two examples that dlopen one cannot
# run. That is the build's decision, not a failure -- but say so out loud,
# because a silent skip is how a real gap hides.
for f in examples/js/*.js examples/*.js; do
    [ -f "$f" ] || continue
    case "$f" in *hello_module.js|*fib_module.js) continue ;; esac  # compiled, not run
    case "$f" in
      *test_fib.js|*test_point.js)
        if [ ! -f examples/point.so ]; then
            printf "  SKIP  %s (no CONFIG_SHARED_LIBS on this platform)\n" "$f"
            continue
        fi ;;
    esac
    run_one "$f" ./dynajs "$f"
done

echo
echo "=== $OS SUMMARY: $pass ok, $fail failed, $tmo timed out, $crash crashed ==="
[ -n "$FAILED" ]   && echo "FAILED:  $FAILED"
[ -n "$TIMEDOUT" ] && echo "TIMEOUT: $TIMEDOUT"
[ -n "$CRASHED" ]  && echo "CRASHED: $CRASHED"
[ $fail = 0 ] && [ $tmo = 0 ] && [ $crash = 0 ]
