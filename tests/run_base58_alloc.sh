#!/bin/sh
# run_base58_alloc.sh -- the ALWAYS-ON allocation-COUNT gate for the base58
# byte paths (engine allocator accounting).
#
# Runs tests/test_base58_alloc.js under the engine's -T allocator (every
# engine-allocator allocation emits one "A <size>" trace line on stdout) and
# counts the allocations strictly between the workload's markers. The
# workload warms its print machinery up front and the baseline region must
# then count EXACTLY 0 -- every row is asserted as a raw count.
#
# COUNTING MODES -- the engine's small-block arenas serve allocations <= ~512
# bytes from free-lists that never reach js_malloc, so "A" lines alone do not
# see pooled traffic. This script runs the workload with DYNAJS_MALLOC_POOLS=0
# (every engine allocation reaches js_malloc and is traced); a binary that
# does not know that switch is detected via the pool-probe region and held to
# the pooled-aware restatement instead. Both modes are exact, no tolerance:
#
# STRICT (pools bypassed -- "A" lines count EVERY engine allocation):
#   base58DecodeInto / base58CheckDecodeInto   0 allocations per call (THE
#       claim: the decode-into byte path touches the engine allocator not at
#       all, over every input length 0..512 and the strided lengths to the
#       4096 cap, for all-zero payloads ('1' text), carry storms ('z' text
#       and 0xFF / 0xFF+0x01 byte runs) and mixed input). The division
#       core's C-heap workspace is NOT the engine allocator:
#       tests/run_base58_strict.sh pins it exactly (2 malloc-family calls
#       per call) so any extra traffic there fails too.
#   Base58Encode / BaseXEncode                 1 per non-empty call (the
#       result string; the empty result is the interned empty string)
#   Base58CheckEncode                          1 per call (even the empty
#       input emits its 4-byte checksum, so every result is non-empty)
#   cal-* twins (one fresh string of the encoder result lengths per call)
#                                              1 per non-empty call
#
# LEGACY (pooled build; "A" lines only show large/unpooled allocations):
#   decode rows                                0 large/unpooled allocations
#   each encoder row                           must equal its cal-* twin
#       (one result-string-shaped allocation per call and nothing else --
#       counts that match pooled reality: results up to ~488 chars are
#       pool-served and legitimately invisible here)
#
# What reddens what: a scratch buffer routed through js_malloc, or a
# temporary added to the byte path at ANY size on a STRICT binary, fails its
# region on the first run. On a LEGACY binary only large/unpooled additions
# are visible -- pooled-size temporaries are caught by the strict rows of
# tests/run_base58_strict.sh (tool-gated) or by this gate against a STRICT
# binary. The claim "a temporary on the argument conversion fails this gate"
# is only true in STRICT mode; do not rely on LEGACY mode for it.
#
# Usage: tests/run_base58_alloc.sh [path-to-dynajs]
set -e
DYNAJS=${1:-./dynajs}
TMP="${TMPDIR:-/tmp}"
TRACE="$TMP/base58_alloc_trace.$$"
trap 'rm -f "$TRACE"' EXIT

DYNAJS_MALLOC_POOLS=0 "$DYNAJS" -T --std tests/test_base58_alloc.js > "$TRACE"

count_region() {
    awk -v b="##ALLOC-BEGIN $1##" '
        $0 == b { p = 1; next }
        $0 == "##ALLOC-END##" { p = 0 }
        p && /^A / { c++ }
        END { print c + 0 }
    ' "$TRACE"
}

# the workload's call accounting (##SAMPLE lens=... nonempty=... ##)
NLENS=$(sed -n 's/^##SAMPLE lens=\([0-9]*\) .*/\1/p' "$TRACE")
NONEMPTY=$(sed -n 's/^##SAMPLE lens=[0-9]* nonempty=\([0-9]*\) .*/\1/p' "$TRACE")
if [ -z "$NLENS" ] || [ -z "$NONEMPTY" ]; then
    echo "run_base58_alloc: no ##SAMPLE line in the workload output"
    exit 1
fi

fail=0

check() {
    # check <region> <expected> <note>  (raw count, since baseline must be 0)
    got=$(count_region "$1")
    if [ "$got" -eq "$2" ]; then
        echo "ok   $1: $got allocations ($3)"
    else
        echo "FAIL $1: $got allocations, want exactly $2 ($3)"
        fail=1
    fi
}

check_eq() {
    # check_eq <region> <other-region> <note>: both must count the same
    got=$(count_region "$1")
    want=$(count_region "$2")
    if [ "$got" -eq "$want" ]; then
        echo "ok   $1: $got allocations, == $2 ($3)"
    else
        echo "FAIL $1: $got allocations, $2 has $want ($3)"
        fail=1
    fi
}

echo "info sweep: $NLENS lengths ($NONEMPTY non-empty), pool zone 0..512 dense"

# the warmup region absorbs the one-time machinery costs; from baseline on,
# the artifact must be EXACTLY zero (raw counts ARE the assertion)
warm=$(count_region warmup)
check baseline 0 "the sweep loops and markers allocate nothing (warmup absorbed $warm one-time allocations)"

probe=$(count_region pool-probe)
if [ "$probe" -gt 0 ]; then
    mode=STRICT
else
    mode=LEGACY
fi
echo "info counting mode: $mode (pool-probe saw $probe allocations; STRICT = every engine allocation traced)"

# the decode-into zero-claim: identical in both modes (0 engine allocations;
# in LEGACY mode the reading is "zero large/unpooled", see the header)
check decode-into-ones  0 "0/call: base58DecodeInto, all-'1' text (all-zero payload)"
check decode-into-zs    0 "0/call: base58DecodeInto, all-'z' text (carry storm)"
check decode-into-mixed 0 "0/call: base58DecodeInto, mixed text"
check check-decode-into 0 "0/call: base58CheckDecodeInto, 3x65 valid-checksum texts"

if [ "$mode" = STRICT ]; then
    check pool-probe 32 "32 tiny encoder calls = 32 result strings"
    check encode-zeros "$NONEMPTY" "1/non-empty-call: Base58Encode, all-zero input"
    check encode-ffs   "$NONEMPTY" "1/non-empty-call: Base58Encode, 0xFF carry-storm input"
    check encode-alt   "$NONEMPTY" "1/non-empty-call: Base58Encode, 0xFF/0x01 carry-storm input"
    check encode-mixed "$NONEMPTY" "1/non-empty-call: Base58Encode, mixed input"
    check check-encode "$NLENS"    "1/call: Base58CheckEncode (checksummed empty is non-empty)"
    check basex-encode "$NONEMPTY" "1/non-empty-call: BaseXEncode, hex alphabet"
    # cal-* twins are report-only here: the strict rows above already pin the
    # exact per-call count (each twin builds one fresh string of the same
    # result length per call, mirroring the encoder's result-string cost)
    for r in cal-zeros cal-ffs cal-alt cal-mixed cal-check cal-basex; do
        echo "info $r: $(count_region $r) allocations (calibration twin, report-only in STRICT mode)"
    done
else
    check pool-probe 0 "32 tiny results are pool-served here (expected 0 large/unpooled)"
    # pooled reality: an encoder row and its cal-* twin (one fresh string of
    # the SAME result length per call) must count identically -- one
    # result-string-shaped allocation per call and nothing else.
    check_eq encode-zeros cal-zeros "one result-string-shaped allocation per call, nothing else"
    check_eq encode-ffs   cal-ffs   "one result-string-shaped allocation per call, nothing else"
    check_eq encode-alt   cal-alt   "one result-string-shaped allocation per call, nothing else"
    check_eq encode-mixed cal-mixed "one result-string-shaped allocation per call, nothing else"
    check_eq check-encode cal-check "one result-string-shaped allocation per call, nothing else"
    check_eq basex-encode cal-basex "one result-string-shaped allocation per call, nothing else"
fi

# report-only: lazy (sliced) argument strings materialize in the engine's
# string machinery at conversion time (NOT the byte path). Non-zero here is
# expected; zero would mean the counting above is an artifact.
sliced=$(count_region decode-into-sliced-info)
echo "info decode-into-sliced-info: $sliced allocations (lazy-string materialization, report-only)"

if [ "$fail" -ne 0 ]; then
    echo "run_base58_alloc: FAILURES (see above)"
    exit 1
fi
echo "run_base58_alloc: all allocation counts exact ($mode mode)"
