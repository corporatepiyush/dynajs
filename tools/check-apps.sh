#!/bin/sh
# Run every program under examples/apps and report which ones fail.
#
#   tools/check-apps.sh [DIR] [path/to/dynajs]
#
# Each app is a complete program that drives itself (a server example starts on
# port 0, calls itself and shuts down), so "it exits 0" is the contract. A file
# whose first line contains "apps:skip" needs an external service and is
# counted as skipped, never silently dropped. Every run has a wall-clock and a
# CPU-time bound, and a timeout is reported distinctly from a failure.
set -u
DIR="${1:-examples/apps}"
BIN="${2:-./dynajs}"
WALL_LIMIT="${CHECK_WALL_LIMIT:-30}"
CPU_LIMIT="${CHECK_CPU_LIMIT:-30}"
OUT="${TMPDIR:-/tmp}/apps-check"

[ -d "$DIR" ] || { echo "no such directory: $DIR" >&2; exit 2; }
[ -x "$BIN" ] || { echo "not executable: $BIN" >&2; exit 2; }
case "$BIN" in /*) ;; *) BIN="$(pwd)/$BIN" ;; esac
rm -rf "$OUT"; mkdir -p "$OUT"

run_app() {
    ( ulimit -t "$CPU_LIMIT" 2>/dev/null; exec "$BIN" "$1" ) > "$2" 2>&1 &
    epid=$!
    # The watcher traps TERM so that cancelling it also ends its sleep; a bare
    # "( sleep N; kill ) &" leaves an orphan sleep behind for every app.
    ( sleep "$WALL_LIMIT" & sp=$!; trap 'kill "$sp" 2>/dev/null; exit 0' TERM; wait "$sp"; kill -9 "$epid" 2>/dev/null ) > /dev/null 2>&1 &
    wpid=$!
    wait "$epid" 2>/dev/null
    rc=$?
    kill "$wpid" 2>/dev/null
    wait "$wpid" 2>/dev/null
    [ "$rc" -gt 128 ] && return 124
    return "$rc"
}

count=0; fail=0; ntimeout=0; nskip=0
for f in "$DIR"/*.js; do
    [ -f "$f" ] || continue
    name=$(basename "$f" .js)
    if head -1 "$f" | grep -q "apps:skip"; then
        nskip=$((nskip + 1)); printf 'skip %s\n' "$name"; continue
    fi
    count=$((count + 1))
    log="$OUT/$name.out"
    if run_app "$f" "$log"; then
        printf 'ok   %s\n' "$name"
    else
        rc=$?
        if [ "$rc" -eq 124 ]; then
            printf 'TIMEOUT %s (killed after %ss)\n' "$name" "$WALL_LIMIT"
            ntimeout=$((ntimeout + 1))
        else
            printf 'FAIL %s\n' "$name"
        fi
        sed 's/^/       /' "$log" | tail -6
        fail=$((fail + 1))
    fi
done

printf '\n%s: %d apps run, %d failed (%d of them timeouts), %d skipped (apps:skip)\n' \
       "$DIR" "$count" "$fail" "$ntimeout" "$nskip"
[ "$count" -gt 0 ] || { echo "no apps found under $DIR" >&2; exit 2; }
[ "$fail" -eq 0 ]
