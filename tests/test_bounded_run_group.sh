#!/usr/bin/env bash
# tests/test_bounded_run_group.sh -- tools/bounded-run.sh must put the command
# in its OWN process group under every /bin/sh it meets, and say nothing about
# it. Under dash (the Linux container's /bin/sh) `set -m` once printed
# "set: can't access tty; job control turned off" at the top of every suite's
# output and made no group, so a helper a suite backgrounded outlived it.
# For each shell present (sh, dash, bash) this proves:
#   1 the command's process group is not the caller's
#   2 a helper the command backgrounded is dead once bounded-run returns
#   3 bounded-run wrote nothing to stderr
set -uo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/br-group.XXXXXX")
trap 'pkill -f "$work/helper" 2>/dev/null; rm -rf "$work"' EXIT
printf '#!/bin/sh\nwhile :; do sleep 1; done\n' > "$work/helper"; chmod +x "$work/helper"
fail=0
note(){ printf '  %-6s %s\n' "$1" "$2"; }
for sh in sh dash bash; do
  command -v "$sh" >/dev/null 2>&1 || { note skip "$sh: not installed"; continue; }
  out="$work/$sh"
  (cd "$work" && "$sh" "$root/tools/bounded-run.sh" 30 30 grp -- /bin/sh -c \
    'ps -o pgid= -p $$ | tr -d " " > pg; "$0" >/dev/null 2>&1 & echo $! > hp' "$work/helper") \
    </dev/null >"$out.out" 2>"$out.err"
  me=$(ps -o pgid= -p $$ | tr -d ' ')
  hp=$(cat "$work/hp" 2>/dev/null); pg=$(cat "$work/pg" 2>/dev/null)
  i=0; while [ -n "$hp" ] && kill -0 "$hp" 2>/dev/null && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
  if [ -n "$pg" ] && [ "$pg" != "$me" ]; then note ok "$sh: the command runs in its own process group"
  else note FAIL "$sh: the command shares the caller's process group ($pg)"; fail=1; fi
  if [ -n "$hp" ] && ! kill -0 "$hp" 2>/dev/null; then note ok "$sh: a backgrounded helper dies with the command"
  else note FAIL "$sh: helper $hp outlived bounded-run"; fail=1; kill -KILL "$hp" 2>/dev/null; fi
  if [ ! -s "$out.err" ]; then note ok "$sh: nothing on stderr"
  else note FAIL "$sh: stderr: $(tr '\n' '|' < "$out.err")"; fail=1; fi
  rm -f "$work/hp" "$work/pg"
done
[ "$fail" = 0 ] && { echo "bounded-run-group: every shell gets its own group, reaps helpers, stays silent"; exit 0; }
echo "bounded-run-group: FAILED"; exit 1
