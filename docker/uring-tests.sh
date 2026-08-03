#!/bin/sh
# The tests NATIVE_TESTS excludes, run against each Linux backend in turn.
# Any failure fails the container, so this is a gate and not a demo.
set -u
cd /src
TESTS="tests/test_uring_disk.js tests/test_http.js tests/test_http_async.js
       tests/test_http_keepalive.js tests/test_http_security.js tests/test_http_ws.js"
rc=0
for backend in epoll uring; do
    exe="/usr/local/bin/dyna-$backend"
    echo "================ backend: $backend ================"
    "$exe" -e 'import("dyna:net")' >/dev/null 2>&1 || {
        echo "FAIL: $backend build cannot load dyna:net"; rc=1; continue; }
    for t in $TESTS; do
        [ -f "$t" ] || { echo "  skip (absent): $t"; continue; }
        # dyna:uring exists only in a CONFIG_IO_URING=y build. Probe for the
        # module rather than matching on the backend name, so this says why it
        # skipped instead of failing closed.
        if grep -q "dyna:uring" "$t" 2>/dev/null && \
           ! "$exe" -e 'import("dyna:uring")' >/dev/null 2>&1; then
            echo "  skip $t (no dyna:uring in the $backend build)"; continue
        fi
        out=$("$exe" "$t" 2>&1); trc=$?
        if [ $trc -eq 0 ]; then
            echo "  ok   $t -- $(echo "$out" | tail -1)"
        else
            echo "  FAIL $t (rc=$trc)"; echo "$out" | tail -5 | sed 's/^/       /'; rc=1
        fi
    done
done
echo "uring-tests: $([ $rc -eq 0 ] && echo ALL PASS || echo FAILURES)"
exit $rc
