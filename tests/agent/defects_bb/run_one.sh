#!/usr/bin/env bash
# run_one.sh ENGINE MATRIX PROBEID  ->  out/<engine>/<matrix>/<id>.txt
# Engine is one of: dynajs (patched), node (oracle), pristine.
set -u
cd "$(dirname "$0")"
ENGINE="$1"; MATRIX="$2"; PID="$3"
ROOT="$(pwd)"
TREE="$ROOT/../../.."
F_JS="probes/$MATRIX/$PID.js"
F_MJS="probes/$MATRIX/$PID.mjs"
FILE=""
MOD=0
if [ -f "$F_MJS" ]; then FILE="$F_MJS"; MOD=1; elif [ -f "$F_JS" ]; then FILE="$F_JS"; fi
OUT="out/$ENGINE/$MATRIX"
mkdir -p "$OUT"
TMPOUT="$OUT/$PID.txt"
if [ -z "$FILE" ]; then
  printf 'MISSING-FILE RC=127\n' > "$TMPOUT"
  exit 0
fi
rc=0
case "$ENGINE" in
  dynajs)
    if [ "$MOD" = "1" ]; then timeout 30 "$TREE/dynajs" --std -m "$FILE" > "$TMPOUT.raw" 2>&1 || rc=$?
    else timeout 30 "$TREE/dynajs" --std "$FILE" > "$TMPOUT.raw" 2>&1 || rc=$?; fi
    ;;
  pristine)
    if [ "$MOD" = "1" ]; then timeout 30 "$TREE/dynajs.pristine" --std -m "$FILE" > "$TMPOUT.raw" 2>&1 || rc=$?
    else timeout 30 "$TREE/dynajs.pristine" --std "$FILE" > "$TMPOUT.raw" 2>&1 || rc=$?; fi
    ;;
  node)
    # --unhandled-rejections=warn: dynajs has no unhandled-rejection detection;
    # this keeps the oracle rc semantics comparable (trace semantics unchanged).
    if [ "$MOD" = "1" ]; then timeout 30 node --unhandled-rejections=warn "$FILE" > "$TMPOUT.raw" 2>&1 || rc=$?
    else timeout 30 node --unhandled-rejections=warn "$FILE" > "$TMPOUT.raw" 2>&1 || rc=$?; fi
    ;;
  *) echo "bad engine" > "$TMPOUT"; exit 1;;
esac
{
  cat "$TMPOUT.raw"
  printf 'RC=%s\n' "$rc"
} > "$TMPOUT"
rm -f "$TMPOUT.raw"
exit 0
