#!/bin/sh
# Run every ```js block in a markdown file and report which ones fail.
#
# A README example that does not run is worse than no example: it is a
# confident-looking lie, and nothing else in the build catches it. This
# extracts each fenced js block to a file and executes it.
#
#   tools/check-readme-examples.sh [FILE.md] [path/to/dynajs]
#
# A block that cannot run standalone -- one that starts a server and never
# returns, or a fragment -- is skipped by putting this immediately above its
# fence, which renders as nothing:
#
#   <!-- check:skip -->
#
# Skipped blocks are counted and printed, so "0 failed" can never quietly mean
# "nothing was run".
#
# Every example runs under a wall-clock and a CPU-time bound, and a block that
# exceeds either is reported as TIMEOUT, distinctly from FAIL. Without that, one
# hanging example hangs the whole gate, which is indistinguishable from a slow
# run -- and a hang is not hypothetical here: a NULL dereference that re-executes
# its faulting instruction spins at 100% forever instead of raising SIGSEGV, so
# it produces no crash, no output, and no exit.

set -u
MD="${1:-README.md}"
BIN="${2:-./dynajs}"
OUT="${TMPDIR:-/tmp}/readme-examples"
WALL_LIMIT="${CHECK_WALL_LIMIT:-20}"    # seconds of wall clock per example
CPU_LIMIT="${CHECK_CPU_LIMIT:-20}"      # seconds of CPU per example

[ -f "$MD" ] || { echo "no such file: $MD" >&2; exit 2; }
[ -x "$BIN" ] || { echo "not executable: $BIN" >&2; exit 2; }

rm -rf "$OUT"; mkdir -p "$OUT"

awk -v out="$OUT" '
  /^<!-- check:skip -->$/ { skipnext=1; next }
  /^```js$/ {
      if (skipnext) { skipping=1; skipnext=0; nskip++; next }
      n++; f=sprintf("%s/ex%02d.js", out, n); inblock=1; next
  }
  skipping && /^```$/ { skipping=0; next }
  skipping            { next }
  inblock && /^```$/  { inblock=0; close(f); next }
  inblock             { print > f }
  { skipnext=0 }
  END { printf "%d %d\n", n+0, nskip+0 > (out "/.count") }
' "$MD"

# Run one example bounded by both limits, output to $2. Returns the example's
# own status, or 124 if it had to be killed.
#
# The watchdog is a background sleep rather than a polling loop so a fast example
# pays nothing: it is cancelled the moment the example exits. ulimit -t is belt
# and braces for the spin case, where the CPU bound trips first and more cheaply.
run_example() {
    ( ulimit -t "$CPU_LIMIT" 2>/dev/null; exec "$BIN" "$1" ) > "$2" 2>&1 &
    epid=$!
    ( sleep "$WALL_LIMIT"; kill -9 "$epid" 2>/dev/null ) > /dev/null 2>&1 &
    wpid=$!
    wait "$epid" 2>/dev/null
    rc=$?
    kill "$wpid" 2>/dev/null
    wait "$wpid" 2>/dev/null
    [ "$rc" -gt 128 ] && return 124
    return "$rc"
}

count=$(cut -d" " -f1 "$OUT/.count" 2>/dev/null || echo 0)
nskip=$(cut -d" " -f2 "$OUT/.count" 2>/dev/null || echo 0)
fail=0
ntimeout=0
i=1
while [ "$i" -le "$count" ]; do
    f=$(printf "%s/ex%02d.js" "$OUT" "$i")
    if [ -f "$f" ]; then
        log="$f.out"
        if run_example "$f" "$log"; then
            printf 'ok   ex%02d\n' "$i"
        else
            rc=$?
            if [ "$rc" -eq 124 ]; then
                printf 'TIMEOUT ex%02d (killed after %ss)\n' "$i" "$WALL_LIMIT"
                ntimeout=$((ntimeout + 1))
            else
                printf 'FAIL ex%02d\n' "$i"
            fi
            sed 's/^/       /' "$log" | head -4
            printf '     --- source ---\n'
            sed 's/^/       /' "$f" | head -14
            fail=$((fail + 1))
        fi
    fi
    i=$((i + 1))
done

printf '\n%s: %d examples run, %d failed (%d of them timeouts), %d skipped (check:skip)\n' \
       "$MD" "$count" "$fail" "$ntimeout" "$nskip"
[ "$fail" -eq 0 ]
