#!/usr/bin/env bash
# Run every example program (examples/<area>/<name>.js) and report which fail.
#
#   tools/check-apps.sh [DIR] [path/to/dynajs]
#
# Each example is a complete program that drives itself when run without
# arguments (a server starts on port 0, calls itself and shuts down; a tool
# builds sample input in a temp directory and checks its own report), so
# "it exits 0" is the contract. The top level of DIR holds the embedding
# fixtures the build compiles; only the area directories hold examples. A file
# whose first line contains "apps:skip" needs an external service and is
# counted as skipped, never silently dropped. A "// flags: ..." line within the
# first three lines passes those interpreter flags (e.g. --std for os.Worker).
# Every run has a wall-clock and a CPU-time bound, and a timeout is reported
# distinctly from a failure. The examples run CHECK_APPS_PAR at a time (default:
# the CPU count); the report is printed afterwards in file order, so it reads
# the same whatever finished first. An example whose first three lines contain
# "apps:solo" measures its own timing (a benchmark, a clock-driven rollout) and
# runs alone, after the pool has drained.
set -u
DIR="${1:-examples}"
BIN="${2:-./dynajs}"
WALL_LIMIT="${CHECK_WALL_LIMIT:-30}"
CPU_LIMIT="${CHECK_CPU_LIMIT:-30}"
OUT="${TMPDIR:-/tmp}/apps-check"
PAR="${CHECK_APPS_PAR:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)}"
case "$PAR" in ''|*[!0-9]*|0) echo "CHECK_APPS_PAR must be a positive integer (got '$PAR')" >&2; exit 2 ;; esac

[ -d "$DIR" ] || { echo "no such directory: $DIR" >&2; exit 2; }
[ -x "$BIN" ] || { echo "not executable: $BIN" >&2; exit 2; }
case "$BIN" in /*) ;; *) BIN="$(pwd)/$BIN" ;; esac
rm -rf "$OUT"; mkdir -p "$OUT"

run_app() {
    flags=$(head -3 "$1" | sed -n 's#^// flags: ##p')
    # shellcheck disable=SC2086
    ( ulimit -t "$CPU_LIMIT" 2>/dev/null; exec "$BIN" $flags "$1" ) > "$2" 2>&1 &
    epid=$!
    # The watcher traps TERM so that cancelling it also ends its sleep; a bare
    # "( sleep N; kill ) &" leaves an orphan sleep behind for every app.
    ( trap 'kill "$sp" 2>/dev/null; exit 0' TERM; sleep "$WALL_LIMIT" & sp=$!; wait "$sp"; kill -9 "$epid" 2>/dev/null ) > /dev/null 2>&1 &
    wpid=$!
    wait "$epid" 2>/dev/null
    rc=$?
    kill "$wpid" 2>/dev/null
    wait "$wpid" 2>/dev/null
    [ "$rc" -gt 128 ] && return 124
    return "$rc"
}

logof() { printf '%s/%s.out' "$OUT" "$(printf '%s' "$1" | tr '/' '_')"; }
nameof() { printf '%s/%s' "$(basename "$(dirname "$1")")" "$(basename "$1" .js)"; }

solo=()
for f in "$DIR"/*/*.js; do
    [ -f "$f" ] || continue
    head -1 "$f" | grep -q "apps:skip" && continue
    if head -3 "$f" | grep -q "apps:solo"; then solo+=("$f"); continue; fi
    while [ "$(jobs -rp | wc -l | tr -d ' ')" -ge "$PAR" ]; do
        wait -n 2>/dev/null || sleep 0.05
    done
    log=$(logof "$(nameof "$f")")
    ( run_app "$f" "$log"; echo "$?" > "$log.rc" ) &
done
wait
for f in ${solo[@]+"${solo[@]}"}; do
    log=$(logof "$(nameof "$f")")
    run_app "$f" "$log"; echo "$?" > "$log.rc"
done

count=0; fail=0; ntimeout=0; nskip=0
for f in "$DIR"/*/*.js; do
    [ -f "$f" ] || continue
    name=$(nameof "$f")
    if head -1 "$f" | grep -q "apps:skip"; then
        nskip=$((nskip + 1)); printf 'skip %s\n' "$name"; continue
    fi
    count=$((count + 1))
    log=$(logof "$name")
    rc=$(cat "$log.rc" 2>/dev/null)
    case "$rc" in ''|*[!0-9]*) rc=125; echo "(no verdict recorded for $name)" >> "$log" ;; esac
    if [ "$rc" -eq 0 ]; then
        printf 'ok   %s\n' "$name"
    else
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

printf '\n%s: %d examples run, %d failed (%d of them timeouts), %d skipped (apps:skip)\n' \
       "$DIR" "$count" "$fail" "$ntimeout" "$nskip"
[ "$count" -gt 0 ] || { echo "no examples found under $DIR/*/" >&2; exit 2; }
[ "$fail" -eq 0 ]
