#!/bin/sh
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

NLENS=$(sed -n 's/^##SAMPLE lens=\([0-9]*\) .*/\1/p' "$TRACE")
NONEMPTY=$(sed -n 's/^##SAMPLE lens=[0-9]* nonempty=\([0-9]*\) .*/\1/p' "$TRACE")
if [ -z "$NLENS" ] || [ -z "$NONEMPTY" ]; then
    echo "run_base58_alloc: no ##SAMPLE line in the workload output"
    exit 1
fi

fail=0

check() {
    got=$(count_region "$1")
    if [ "$got" -eq "$2" ]; then
        echo "ok   $1: $got allocations ($3)"
    else
        echo "FAIL $1: $got allocations, want exactly $2 ($3)"
        fail=1
    fi
}

check_eq() {
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

warm=$(count_region warmup)
check baseline 0 "the sweep loops and markers allocate nothing (warmup absorbed $warm one-time allocations)"

probe=$(count_region pool-probe)
if [ "$probe" -gt 0 ]; then
    mode=STRICT
else
    mode=LEGACY
fi
echo "info counting mode: $mode (pool-probe saw $probe allocations; STRICT = every engine allocation traced)"

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
    for r in cal-zeros cal-ffs cal-alt cal-mixed cal-check cal-basex; do
        echo "info $r: $(count_region $r) allocations (calibration twin, report-only in STRICT mode)"
    done
else
    check pool-probe 0 "32 tiny results are pool-served here (expected 0 large/unpooled)"
    check_eq encode-zeros cal-zeros "one result-string-shaped allocation per call, nothing else"
    check_eq encode-ffs   cal-ffs   "one result-string-shaped allocation per call, nothing else"
    check_eq encode-alt   cal-alt   "one result-string-shaped allocation per call, nothing else"
    check_eq encode-mixed cal-mixed "one result-string-shaped allocation per call, nothing else"
    check_eq check-encode cal-check "one result-string-shaped allocation per call, nothing else"
    check_eq basex-encode cal-basex "one result-string-shaped allocation per call, nothing else"
fi

sliced=$(count_region decode-into-sliced-info)
echo "info decode-into-sliced-info: $sliced allocations (lazy-string materialization, report-only)"

if [ "$fail" -ne 0 ]; then
    echo "run_base58_alloc: FAILURES (see above)"
    exit 1
fi
echo "run_base58_alloc: all allocation counts exact ($mode mode)"
