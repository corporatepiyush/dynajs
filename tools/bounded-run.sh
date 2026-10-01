#!/bin/sh
# tools/bounded-run.sh -- external wall-clock AND CPU-time bound for one gate
# run, reporting a TIMEOUT distinctly from a failure.
#
# Two instruments, because a run can wedge two ways and only one of them burns
# CPU: a wedged syscall / lost reply parks the process at 0% CPU (caught by
# the WALL bound), while a synchronous busy-spin in native code blocks the JS
# event loop at 100% of one core -- so an in-test, loop-driven watchdog never
# fires (caught by the CPU bound). Neither bound can be reached from inside
# the process it bounds; that is the point of an external bound.
#
# Sizes are chosen well above the in-test watchdogs (e.g. 20s of continuous
# zero progress in test_net_fdchurn.js) so a slow-but-progressing machine is
# never killed while a genuine hang still dies in bounded time.
#
# Usage: bounded-run.sh <wall_s> <cpu_s> <label> -- <command> [args...]
#   wall_s  hard wall-clock bound, seconds (> 0)
#   cpu_s   CPU-time bound, seconds, applied via ulimit -t (> 0)
#   label   name used in the TIMEOUT report
#   --      separator; everything after it is the command
#
# Exit status:
#   the command's own status when it finishes inside both bounds;
#   124  TIMEOUT (wall clock) -- the command was terminated;
#   125  TIMEOUT (CPU time)   -- the command died on SIGXCPU/ulimit.
# A TIMEOUT never masquerades as the command's own failure exit.
#
# Callers that must LABEL a 124/125 honestly (a suite may exit 124 itself)
# export BOUNDED_RUN_MARKER=<path>: the file is created only when one of the
# bounds above actually fired, so "external bound" is a fact the caller can
# check instead of an inference from the exit code. Without the marker, a
# command that exits 124/125 on its own is reported as such on stderr and its
# status is passed through unchanged.
set -u

if [ "$#" -lt 5 ] || [ "$4" != "--" ]; then
    echo "usage: $0 <wall_s> <cpu_s> <label> -- <command> [args...]" >&2
    exit 2
fi
wall=$1
cpu=$2
label=$3
shift 4

case $wall in *[!0-9]*|'') echo "bounded-run: wall_s must be a positive integer" >&2; exit 2;; esac
case $cpu  in *[!0-9]*|'') echo "bounded-run: cpu_s must be a positive integer" >&2; exit 2;; esac
[ "$wall" -gt 0 ] && [ "$cpu" -gt 0 ] || {
    echo "bounded-run: bounds must be positive" >&2; exit 2; }

ulimit -t "$cpu" 2>/dev/null || true

mkdir -p .obj 2>/dev/null || true
marker=".obj/.bounded-run-$$"
rm -f "$marker"

ext_marker=${BOUNDED_RUN_MARKER:-}
[ -n "$ext_marker" ] && rm -f "$ext_marker" 2>/dev/null

"$@" &
cmdpid=$!
(
    sleep "$wall"
    : > "$marker"
    kill -TERM "$cmdpid" 2>/dev/null
    sleep 5
    kill -KILL "$cmdpid" 2>/dev/null
) &
watchdog=$!

wait "$cmdpid"
rc=$?

kill "$watchdog" 2>/dev/null
wait "$watchdog" 2>/dev/null

if [ -f "$marker" ] && [ "$rc" -ne 0 ]; then
    rm -f "$marker"
    [ -n "$ext_marker" ] && : > "$ext_marker" 2>/dev/null
    echo "TIMEOUT: $label exceeded the ${wall}s wall-clock bound and was terminated (a wedged syscall or a lost reply parks the run at 0% CPU where an in-test watchdog cannot fire)" >&2
    exit 124
fi
rm -f "$marker"

if [ "$rc" -eq 152 ] || [ "$rc" -eq 137 ]; then
    [ -n "$ext_marker" ] && : > "$ext_marker" 2>/dev/null
    echo "TIMEOUT: $label exceeded the ${cpu}s CPU-time bound and was terminated (a synchronous busy-spin blocks the event loop where an in-test watchdog cannot fire)" >&2
    exit 125
fi

if [ "$rc" -eq 124 ] || [ "$rc" -eq 125 ]; then
    echo "bounded-run: the command exited $rc on its own -- NOT killed by the external bound" >&2
fi

exit "$rc"
