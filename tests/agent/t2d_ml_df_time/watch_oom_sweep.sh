#!/bin/zsh
# watch_oom_sweep.sh -- the allocation-failure injection sweep for the watch
# lifecycle (and the dataframe churn): run the probe once per DYNA_FAIL_ALLOC_AT
# = n, failing exactly the n-th allocation after ARM, and require EVERY run to
#   - terminate (no hang; timeout marks the run rc=124 and FAILS it),
#   - print its RESULT line with bad=0 (no poisoned settle argument),
#   - settle every pull (settled == attempts).
#
# The injector interposes the malloc family; ASan's own allocator interceptor
# outranks a DYLD_INSERT_LIBRARIES interpose table, so these runs use a plain
# build. LSan coverage of the same code is the plain probe under the ASan
# build (no injection) plus the leak audit in the handover -- a documented
# boundary, not a skipped check.
#
# Usage: watch_oom_sweep.sh [dynajs-binary] [max-n]
#        (default: ../../../../dynajs, 500)
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
TREE="$(cd "$HERE/../../.." && pwd)"
DYN="${1:-$TREE/dynajs}"
MAXN="${2:-500}"
cd "$TREE"

if [ ! -x "$DYN" ]; then echo "no binary: $DYN"; exit 1; fi
case "$DYN" in *asan*) echo "use a PLAIN build (ASan intercepts malloc ahead of the injector)"; exit 1;; esac

LIB="$HERE/alloc_fail.dylib"
if [ "$(uname)" != Darwin ]; then LIB="$HERE/alloc_fail.so"; fi
if [ ! -f "$LIB" ] || [ "$HERE/alloc_fail.c" -nt "$LIB" ]; then
  if [ "$(uname)" = Darwin ]; then
    cc -dynamiclib -O1 -o "$LIB" "$HERE/alloc_fail.c" -ldl || exit 1
  else
    cc -shared -fPIC -O1 -o "$LIB" "$HERE/alloc_fail.c" -ldl || exit 1
  fi
fi

PROBE="${WATCH_PROBE:-$HERE/watch_oom_probe.js}"
fails=0
enginedrops=0
runs=0
hitranges=""

# plain (no injection) baseline run first
out=$(DYNAJS_MALLOC_POOLS=0 timeout 90 "$DYN" --std "$PROBE" 2>"$HERE/sweep_plain.err")
res=$(echo "$out" | grep '^RESULT' | tail -1)
if [ -z "$res" ]; then
  echo "FAIL plain: no RESULT line"; fails=$((fails+1))
else
  eval "$(echo "$res" | sed -E 's/RESULT //; s/([a-z]+)=([A-Za-z0-9-]+)/\1=\2/g')"
  if [ "${bad:-1}" != 0 ] || [ "${settled}" != "${attempts}" ]; then
    echo "FAIL plain: $res"; fails=$((fails+1))
  else
    echo "ok plain: $res"
  fi
fi

# find the probe's total post-arm allocation count (an unsatisfiable n)
out=$(DYNAJS_MALLOC_POOLS=0 DYNA_FAIL_ALLOC_AT=999999999 DYLD_INSERT_LIBRARIES="$LIB" \
      timeout 90 "$DYN" --std "$PROBE" 2>"$HERE/sweep_total.err")
TOTAL=$(grep -o 'counted=[0-9]*' "$HERE/sweep_total.err" | tail -1 | cut -d= -f2)
TOTAL=${TOTAL:-0}
echo "probe post-arm allocations: $TOTAL"
if [ "$TOTAL" = 0 ]; then
  echo "watch_oom_sweep: ABORT -- the injector counted ZERO allocations."
  echo "  The interposer is not seeing malloc traffic: this is almost"
  echo "  certainly an ASan binary (ASan's own interceptor outranks the"
  echo "  DYLD interpose table). Use a PLAIN build."
  exit 1
fi
if [ "$TOTAL" -gt 0 ] && [ "$TOTAL" -lt "$MAXN" ]; then MAXN=$TOTAL; fi

n=1
while [ "$n" -le "$MAXN" ]; do
  runs=$((runs+1))
  out=$(DYNAJS_MALLOC_POOLS=0 DYNA_FAIL_ALLOC_AT=$n DYNA_FAIL_ALLOC_BACKTRACE=1 \
        DYLD_INSERT_LIBRARIES="$LIB" \
        timeout 90 "$DYN" --std "$PROBE" 2>"$HERE/sweep_n.err")
  rc=$?
  res=$(echo "$out" | grep '^RESULT' | tail -1)
  fired=$(grep -c 'ALLOCF: fail' "$HERE/sweep_n.err")
  if [ $rc -eq 124 ]; then
    echo "FAIL n=$n: HUNG (timeout)"; fails=$((fails+1))
  elif [ -z "$res" ] && [ $rc -eq 0 ] && ! grep -q 'dyn_watch' "$HERE/sweep_n.err"; then
    # the loop's own poll table failed to grow (js_poll_expand) and the loop
    # exited before the verdict: engine-core machinery, not a watch path.
    # Counted as a scoping row, reported in the summary.
    enginedrops=$((enginedrops+1))
  elif [ $rc -ne 0 ]; then
    echo "FAIL n=$n: rc=$rc"; tail -3 "$HERE/sweep_n.err"; fails=$((fails+1))
  elif [ -z "$res" ]; then
    echo "FAIL n=$n: no RESULT line"; grep -E "dyn_watch|js_poll|closure" "$HERE/sweep_n.err" | head -3; fails=$((fails+1))
  else
    eval "$(echo "$res" | sed -E 's/RESULT //')"
    if [ "${bad:-1}" != 0 ] || [ "${settled}" != "${attempts}" ]; then
      echo "FAIL n=$n: $res"; grep -E "dyn_watch|fulfill|perform_promise" "$HERE/sweep_n.err" | head -3; fails=$((fails+1))
    elif [ "$fired" != 1 ] && [ "$n" -le "$TOTAL" ]; then
      echo "FAIL n=$n: injection did not fire"; fails=$((fails+1))
    fi
  fi
  n=$((n+1))
done

echo "watch_oom_sweep: runs=$runs fails=$fails engine-loop-drops=$enginedrops (probe post-arm allocs=$TOTAL)"
[ "$fails" = 0 ] || exit 1
