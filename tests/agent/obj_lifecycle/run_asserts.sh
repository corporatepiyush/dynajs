#!/bin/sh
# Diff-based assert runner: run both binaries over the probe set and compare
# stdout+stderr+rc byte-for-byte. usage: sh run_asserts.sh <candidate>
cd "$(dirname "$0")/../../.." || exit 1
CAND="${1:-./dynajs}"
PRI=./dynajs.pristine
OUT=tests/agent/obj_lifecycle/out
rc=0
for probe in assert_literal_semantics asan_adversarial; do
  LC_ALL=C $PRI tests/agent/obj_lifecycle/$probe.js > $OUT/$probe.pri.txt 2>&1; rp=$?
  LC_ALL=C $CAND tests/agent/obj_lifecycle/$probe.js > $OUT/$probe.cand.txt 2>&1; rc_=$?
  if [ $rp -ne $rc_ ]; then echo "FAIL $probe rc: pri=$rp cand=$rc_"; rc=1; fi
  if cmp -s $OUT/$probe.pri.txt $OUT/$probe.cand.txt; then
    echo "OK $probe (byte-identical)"
  else
    echo "FAIL $probe output differs:"; diff $OUT/$probe.pri.txt $OUT/$probe.cand.txt | head -20; rc=1
  fi
done
exit $rc
