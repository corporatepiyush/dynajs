#!/bin/bash
# run.sh — INDEPENDENT review-lane runner for tests/agent/dyna_sys_review.
# Runs every probes/**/*.js against the in-tree dynajs (patched) using the
# shared dyna_sys control-server harness (ephemeral 127.0.0.1 ports only),
# then prints a summary. Exit nonzero on any failure.
#   PRISTINE=1 run.sh additionally re-runs everything against dynajs.pristine
#   for before/after columns (pristine failures are EXPECTED for fixed bugs).
set -u
cd "$(dirname "$0")"
ROOT=$(cd ../../.. && pwd)
DYNAJS_BIN="${DYNAJS_BIN:-$ROOT/dynajs}"
OUTDIR="${OUTDIR:-out/js}"
TMO="${TMO:-90}"
JOBS="${JOBS:-6}"
mkdir -p "$OUTDIR" scratch

LIST=$(find probes -name '*.js' | sort)
N=$(echo "$LIST" | grep -c . || true)
echo "run.sh: $N review probes, engine=$DYNAJS_BIN" >&2

FAIL=0
for p in $LIST; do
  base=$(basename "$p" .js)
  ctl=$(head -1 "$p" | sed -n 's/^\/\/ CTL:\([a-z0-9-]*\).*/\1/p')
  outtxt="$OUTDIR/$base.txt"
  if [ -n "$ctl" ]; then
    (cd ../dyna_sys && timeout "$TMO" bash run_one.sh "$ROOT/tests/agent/dyna_sys_review/$p" \
       review "$DYNAJS_BIN" "$ROOT/tests/agent/dyna_sys_review/$OUTDIR" "$TMO" > /dev/null 2>&1)
    rc=$?
  else
    timeout "$TMO" "$DYNAJS_BIN" "$p" > "$outtxt" 2>&1
    rc=$?
  fi
  # ctl mode writes into OUTDIR/<base>.txt via run_one.sh (dir = OUTDIR)
  if [ -n "$ctl" ] && [ ! -f "$outtxt" ]; then outtxt="$ROOT/tests/agent/dyna_sys_review/$OUTDIR/$base.txt"; fi
  summ=$(grep -m1 '^SUMMARY ' "$outtxt" 2>/dev/null || echo "SUMMARY none pass=0 fail=0")
  res=$(grep -m1 '^RESULT ' "$outtxt" 2>/dev/null | awk '{print $2}')
  res=${res:-CRASH}
  printf "%-28s rc=%-4d %-24s %s\n" "$base" "$rc" "$summ" "$res"
  [ "$rc" -ne 0 -o "$res" != "PASS" ] && FAIL=$((FAIL+1))
done
echo "run.sh: $FAIL failing probe(s)"
exit $([ "$FAIL" -eq 0 ] && echo 0 || echo 1)
