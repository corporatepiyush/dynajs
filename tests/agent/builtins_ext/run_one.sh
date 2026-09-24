#!/bin/bash
# run_one.sh "<file>" <tzi> — one battery unit: run (file × TZ) on the needed
# engines, compare with tag whitelist, print one verdict line.
# Collision-free under parallelism: all artifacts keyed by file+tzshort+eng.
set -u
cd "$(dirname "$0")"
ROOT=/Users/piyush/ai/dynascript/.agent-work/work20
DYN="$ROOT/dynajs"
BASE=/tmp/pip/opt/dynajs_baseline
NODE="$(command -v node)"
OUT=results

FRAGDIR="${FRAGDIR:-fragments}"
f="$1"; tzi="$2"
TZS=(UTC America/New_York Asia/Kolkata)
TZNAME=(UTC NY KOL)
tz="${TZS[$tzi]}"; short="${TZNAME[$tzi]}"

mode=full
case "$f" in perf_*) mode=perf;; self_*) mode=self;; esac

okall=1
verdict=DIFF
details=()

if [ "$mode" = perf ]; then
  # perf mode: TREE-ONLY. PASS iff completes cleanly AND every VERDICT line
  # is =OK. RAW timings are informational and engine-local; cross-engine
  # numbers for the report come from manual runs (see findings).
  TZ="$tz" "$DYN" "$f" > "$OUT/$f.$short.dyn.out" 2> "$OUT/$f.$short.dyn.err"
  echo $? > "$OUT/$f.$short.dyn.rc"
  rcd=$(cat "$OUT/$f.$short.dyn.rc")
  if [ "$rcd" = 0 ] && [ ! -s "$OUT/$f.$short.dyn.err" ] && ! grep -q "VERDICT:.*=BAD" "$OUT/$f.$short.dyn.out"; then
    verdict=PASS
  else
    verdict=DIFF; okall=0
    details+=("PERF rc=$rcd $(grep 'VERDICT' "$OUT/$f.$short.dyn.out" | tr '\n' ' ')")
  fi
elif [ "$mode" = self ]; then
  # tree-only self-pin: must exit 0 with empty stderr on the TREE binary
  TZ="$tz" "$DYN" "$f" > "$OUT/$f.$short.dyn.out" 2> "$OUT/$f.$short.dyn.err"
  echo $? > "$OUT/$f.$short.dyn.rc"
  rcd=$(cat "$OUT/$f.$short.dyn.rc")
  if [ "$rcd" = 0 ] && [ ! -s "$OUT/$f.$short.dyn.err" ]; then
    verdict=PASS
  else
    verdict=DIFF; okall=0
    details+=("SELF-PIN rc=$rcd err=$(head -c 200 "$OUT/$f.$short.dyn.err")")
  fi
else
  for eng in dyn node base; do
    case "$eng" in
      dyn) bin="$DYN";;
      node) bin="$NODE";;
      base) bin="$BASE";;
    esac
    TZ="$tz" "$bin" "$f" > "$OUT/$f.$short.$eng.out" 2> "$OUT/$f.$short.$eng.err"
    echo $? > "$OUT/$f.$short.$eng.rc"
    if [ -s "$OUT/$f.$short.$eng.err" ]; then
      okall=0; details+=("$eng-STDERR:$(head -c 200 "$OUT/$f.$short.$eng.err" | tr '\n' ' ')")
    fi
  done
  rcd=$(cat "$OUT/$f.$short.dyn.rc"); rcn=$(cat "$OUT/$f.$short.node.rc"); rcb=$(cat "$OUT/$f.$short.base.rc")
  if [ "$rcd" != "$rcn" ] || [ "$rcd" != "$rcb" ]; then
    okall=0; details+=("rc dyn=$rcd node=$rcn base=$rcb")
  fi
  if [ "$okall" = 1 ]; then
    v_n="$(python3 compare.py "$mode" "$OUT/$f.$short.dyn.out" "$OUT/$f.$short.node.out" node)"
    v_b="$(python3 compare.py "$mode" "$OUT/$f.$short.dyn.out" "$OUT/$f.$short.base.out" base)"
    case "$v_n" in OK) :;; *) okall=0; details+=("vs-node: $v_n");; esac
    case "$v_b" in OK) :;; *) okall=0; details+=("vs-base: $v_b");; esac
    if [ "$okall" = 1 ]; then verdict=PASS; fi
  fi
fi
printf '%-8s %-44s %-6s %s\n' "$verdict" "$f" "$short" "${details[*]:-}" >> "$FRAGDIR/frag.$$"
printf '%-8s %-44s %-6s %s\n' "$verdict" "$f" "$short" "${details[*]:-}"
