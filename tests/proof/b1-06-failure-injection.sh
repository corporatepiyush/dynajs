#!/usr/bin/env bash
# tests/proof/b1-06-failure-injection.sh -- PROVE that a failing command inside
# a stage now turns that stage red (B1-06: "places where a stage reports ok
# without its command actually succeeding").
#
# The finding was established by READING build.sh. This executes it: it makes a
# scratch copy of the tree (so the workspace is never touched), injects one
# deliberate failure into each stage, and requires the stage to exit nonzero.
#
# Four injections, one per failure class that was reported green:
#   1 tls       the FIRST of its two legs fails (g_stage_tls used to return the
#               LAST command's status, so a failing test-x509 was invisible)
#   2 security  a suite in SECURITY_TESTS_TABLE exits nonzero
#   3 api       a suite in API_TESTS_TABLE exits nonzero
#   4 native    a suite in the native list exits nonzero
#
# Usage: tests/proof/b1-06-failure-injection.sh <source-tree>
set -uo pipefail
SRC="${1:?usage: b1-06-failure-injection.sh <source-tree>}"
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SCRATCH=$(mktemp -d "${TMPDIR:-/tmp}/b106.XXXXXX")
trap 'rm -rf "$SCRATCH"' EXIT INT TERM

echo "== building a scratch copy of the tree in $SCRATCH"
mkdir -p "$SCRATCH/tree"
( cd "$SRC" && tar cf - --exclude=.obj --exclude=.obj-san --exclude=.obj-t262 \
      --exclude=.git --exclude=third_party --exclude=test262 . ) \
  | ( cd "$SCRATCH/tree" && tar xf - ) || { echo "FAIL: could not copy the tree"; exit 2; }
ln -s "$SRC/test262" "$SCRATCH/tree/test262" 2>/dev/null
cd "$SCRATCH/tree" || exit 2

fails=0
inject(){ # $1 = file to append a failing suite to
  cat > "$1" <<'EOF'
// B1-06 PROOF: this suite always fails.
print("FAIL: injected failure (B1-06 proof)");
std.exit(1);
EOF
}

expect_red(){ # $1 = stage, $2 = what we injected
  local stage="$1" what="$2" rc
  ./build.sh __gate-stage "$stage" >"$SCRATCH/$stage.log" 2>&1
  rc=$?
  if [ "$rc" != 0 ]; then
    echo "  ok   stage '$stage' went RED (rc=$rc) when $what failed"
  else
    echo "  FAIL stage '$stage' reported OK (rc=0) despite $what failing"
    tail -6 "$SCRATCH/$stage.log" | sed 's/^/       /'
    fails=$((fails+1))
  fi
}

echo "== building the release binary the suite stages need"
./build.sh build CONFIG_NATIVE_MODULES=y CONFIG_TLS=y >"$SCRATCH/build.log" 2>&1 \
  || { echo "FAIL: the scratch tree would not build"; tail -20 "$SCRATCH/build.log"; exit 2; }

echo "== 1 tls: the FIRST leg (test-x509) fails"
# test-x509 is in MT_STANDALONE_TABLE, so no other stage runs it; if g_stage_tls
# only reported the last leg's status, this is invisible.
cp tests/test_x509.js "$SCRATCH/test_x509.orig"
inject tests/test_x509.js
expect_red tls "test-x509"
cp "$SCRATCH/test_x509.orig" tests/test_x509.js

echo "== 2 security: a SECURITY_TESTS_TABLE suite exits nonzero"
cp tests/test_parser_pentest.js "$SCRATCH/parser_pentest.orig"
inject tests/test_parser_pentest.js
expect_red security "a security-table suite"
cp "$SCRATCH/parser_pentest.orig" tests/test_parser_pentest.js

echo "== 3 api: an API_TESTS_TABLE suite exits nonzero"
cp tests/test_api_vectors.js "$SCRATCH/api_vectors.orig"
inject tests/test_api_vectors.js
expect_red api "an api-table suite"
cp "$SCRATCH/api_vectors.orig" tests/test_api_vectors.js

echo "== 4 native: a suite in the native matrix exits nonzero"
cp tests/test_sys.js "$SCRATCH/test_sys.orig"
inject tests/test_sys.js
expect_red native "a native-matrix suite"
cp "$SCRATCH/test_sys.orig" tests/test_sys.js

echo "== 5 control: with the injections reverted every stage is green again"
./build.sh __gate-stage tls >"$SCRATCH/tls-ctl.log" 2>&1 \
  && echo "  ok   stage 'tls' is green again after the revert" \
  || { echo "  FAIL stage 'tls' is still red after the revert"; fails=$((fails+1)); }

echo
if [ "$fails" -eq 0 ]; then
  echo "b1-06-failure-injection: every injected failure turned its stage red"
  exit 0
fi
echo "b1-06-failure-injection: $fails injection(s) stayed green"
exit 1