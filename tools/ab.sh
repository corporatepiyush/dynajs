#!/bin/sh
# ab.sh -- interleaved A/B of two binaries over one or more benchmarks.
#
# Usage:
#   tools/ab.sh BASE_BIN HEAD_BIN bench1.js [bench2.js ...]
#   ROUNDS=5 OUT=/tmp/myab tools/ab.sh ./old ./dynajs tests/bench_string_eq.js
#
# WHY INTERLEAVED. All-base-then-all-head attributes any drift in machine load
# to the change. Alternating base/head per round makes the drift hit both sides
# equally. This matters more than it sounds: measuring while other work shares
# the machine produced a table in which a real 1.5x win read as a 0.94x
# regression across every affected row.
#
# WHAT IT REFUSES TO DO. It will not measure two binaries that hash the same --
# equal hashes mean the build never picked up the change and the whole table is
# one binary against itself, which is a conclusion rather than an error
# message. Run tools/ab-parity.js on both first when the baseline came from a
# fresh checkout: anything the ignore rules exclude (a vendored libm, a
# generated table) is ABSENT there and silently changes what gets built.
#
# The bench FILE is the same in both runs; only the binary differs. Point both
# at the current tree so a bench fixed today can measure a binary built
# yesterday.
set -eu

[ $# -ge 3 ] || { echo "usage: $0 BASE_BIN HEAD_BIN bench.js [...]" >&2; exit 2; }
BASE=$1; HEAD=$2; shift 2
OUT=${OUT:-/tmp/ab-$$}
ROUNDS=${ROUNDS:-3}
mkdir -p "$OUT"

[ -x "$BASE" ] || { echo "FAIL: $BASE is not executable" >&2; exit 1; }
[ -x "$HEAD" ] || { echo "FAIL: $HEAD is not executable" >&2; exit 1; }

hb=$(shasum -a 256 "$BASE" | cut -d' ' -f1)
hh=$(shasum -a 256 "$HEAD" | cut -d' ' -f1)
if [ "$hb" = "$hh" ]; then
    echo "FAIL: both binaries hash $hb -- the experiment never ran" >&2
    exit 1
fi
echo "base=$hb  $BASE"
echo "head=$hh  $HEAD"
echo "rounds=$ROUNDS  out=$OUT"

for b in "$@"; do
    n=$(basename "$b" .js)
    echo "--- $n ---"
    r=1
    while [ "$r" -le "$ROUNDS" ]; do
        timeout 900 "$BASE" "$b" > "$OUT/$n.base.$r.txt" 2>&1
        echo "  base r$r exit=$?"
        timeout 900 "$HEAD" "$b" > "$OUT/$n.head.$r.txt" 2>&1
        echo "  head r$r exit=$?"
        r=$((r + 1))
    done
done
echo "ALL_DONE  results in $OUT"
