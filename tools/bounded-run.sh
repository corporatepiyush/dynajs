#!/bin/sh
# shellcheck disable=SC3045  # ulimit -t is supported by dash, bash and busybox ash: every shell this runs under
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
# zero progress in the fdchurn suites) so a slow-but-progressing machine is
# never killed while a genuine hang still dies in bounded time.
#
# Usage: bounded-run.sh <wall_s> <cpu_s> <label> -- <command> [args...]
#   wall_s  hard wall-clock bound, seconds (> 0)
#   cpu_s   CPU-time bound, seconds, applied via ulimit -t (> 0)
#   label   name used in the TIMEOUT report
#   --      separator; everything after it is the command
# The command's stdin is /dev/null; BOUNDED_RUN_STDIN=1 passes the caller's stdin (a pipe into the bounded command).
#
# Exit status:
#   the command's own status when it finishes inside both bounds;
#   124  TIMEOUT (wall clock) -- the command was terminated;
#   125  TIMEOUT (CPU time)   -- the command died on SIGXCPU/ulimit.
# A TIMEOUT never masquerades as the command's own failure exit.
#
# The command runs in its OWN process group (set -m), and that whole group is
# TERMed (then KILLed after 3 s) when the command ends for ANY reason: a
# normal exit, a failure, either bound, or this script being interrupted
# (INT/TERM/HUP). A helper the suite backgrounded (`sh -c './dynajs mock.js &'`)
# is reparented to pid 1 the moment its shell returns, so only the group still
# names it; killing just the direct children left such helpers alive for days.
#
# Callers that must LABEL a 124/125 honestly (a suite may exit 124 itself)
# export BOUNDED_RUN_MARKER=<path>: the file is created only when one of the
# bounds above actually fired, so "external bound" is a fact the caller can
# check instead of an inference from the exit code. Without the marker, a
# command that exits 124/125 on its own is reported as such on stderr and its
# status is passed through unchanged.
set -u

# The process group needs a shell whose `set -m` works without a terminal:
# dash (Debian/Ubuntu /bin/sh) prints "set: can't access tty; job control
# turned off" into every suite's output and makes NO group, so the reap below
# would miss every helper a suite backgrounded. Re-run under bash when it
# exists; without it (busybox), the direct-children fallback still applies.
if [ -z "${BASH_VERSION:-}" ] && [ -z "${BOUNDED_RUN_NO_BASH:-}" ] && command -v bash >/dev/null 2>&1; then
    exec bash "$0" "$@"
fi

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

[ -d .obj ] || mkdir -p .obj 2>/dev/null || true
marker=".obj/.bounded-run-$$"
rm -f "$marker"

ext_marker=${BOUNDED_RUN_MARKER:-}
[ -n "$ext_marker" ] && rm -f "$ext_marker" 2>/dev/null

reap_group() {
    kill -TERM -"$cmdpid" 2>/dev/null || return 0
    i=0
    while kill -0 -"$cmdpid" 2>/dev/null && [ "$i" -lt 30 ]; do sleep 0.1; i=$((i + 1)); done
    kill -KILL -"$cmdpid" 2>/dev/null
    return 0
}

set -m 2>/dev/null
if [ -n "${BOUNDED_RUN_STDIN:-}" ]; then "$@" <&0 & else "$@" </dev/null & fi
cmdpid=$!
set +m
(
    sp=
    trap '[ -n "$sp" ] && kill "$sp" 2>/dev/null; exit 0' TERM
    sleep "$wall" &
    sp=$!
    wait "$sp"
    : > "$marker"
    # TERM the command's whole process group first: a suite that started a
    # server or a helper would otherwise survive the bound and hold a port the
    # NEXT suite needs (B1-06.12 -- only the direct child was killed). The
    # direct children are signalled too, for a shell where set -m made no group.
    kill -TERM -"$cmdpid" 2>/dev/null
    for p in $(pgrep -P "$cmdpid" 2>/dev/null); do
        kill -TERM "$p" 2>/dev/null
    done
    kill -TERM "$cmdpid" 2>/dev/null
    sleep 5
    kill -KILL -"$cmdpid" 2>/dev/null
    for p in $(pgrep -P "$cmdpid" 2>/dev/null); do
        kill -KILL "$p" 2>/dev/null
    done
    kill -KILL "$cmdpid" 2>/dev/null
) &
watchdog=$!
trap 'reap_group; kill "$watchdog" 2>/dev/null; rm -f "$marker"; exit 130' INT TERM HUP

wait "$cmdpid"
rc=$?
reap_group

kill "$watchdog" 2>/dev/null
wait "$watchdog" 2>/dev/null

# The wall bound fired -> TIMEOUT, ALWAYS. A child that traps SIGTERM and
# exits 0 used to pass after EXCEEDING the bound, so an over-running suite was
# reported as a clean run (B1-06.12).
if [ -f "$marker" ]; then
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
