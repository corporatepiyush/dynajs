#!/usr/bin/env bash
# test262 gate. Usage: t262_gate.sh <tag>  (run-test262 must already be built
# from the engine under test; log expected at test262_<tag>.out, produced by
#   ./run-test262 -c tools/test262.conf -a > test262_<tag>.out 2>&1 ).
# Canonical fail pin: progress-char runs stripped anywhere, free-text message
# cut (both interleave nondeterministically with the progress stream).
# Preflights hard-fail when the log or the base pins are missing -- a gate
# that cannot run must fail loudly, not print "DIFFERS".
cd "$(dirname "$0")/../.." || exit 2
TAG="$1"
[ -n "$TAG" ] || { echo "usage: t262_gate.sh <tag>   (log at test262_<tag>.out)"; exit 2; }
LOG="test262_${TAG}.out"
[ -f "$LOG" ] || { echo "FAIL: $LOG not found -- run: ./run-test262 -c tools/test262.conf -a > $LOG 2>&1"; exit 2; }
[ -s test262_base.pin ] || { echo "FAIL: test262_base.pin missing/empty (baseline fail pin)"; exit 2; }
[ -f test262_base.count ] || { echo "FAIL: test262_base.count missing (baseline counter pin)"; exit 2; }
[ -x tools/agent/extract_t262_fails.sh ] || { echo "FAIL: tools/agent/extract_t262_fails.sh missing"; exit 2; }
cnt=$(grep -oE "[0-9]+/[0-9]+/[0-9]+$" "$LOG" | tail -1)
tools/agent/extract_t262_fails.sh "$LOG" > "test262_${TAG}.pin"
base_cnt=$(cat test262_base.count)
rc=0
if [ "$cnt" = "$base_cnt" ]; then
  echo "counter: $cnt (matches base pin)"
else
  echo "counter: $cnt DIFFERS from base pin $base_cnt"
  rc=1
fi
if cmp -s test262_base.pin "test262_${TAG}.pin"; then
  echo "fail-list: IDENTICAL ($(wc -l < test262_base.pin) pins)"
else
  echo "fail-list: DIFFERS"
  diff test262_base.pin "test262_${TAG}.pin" | head -10
  rc=1
fi
exit $rc
