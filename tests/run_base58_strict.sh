#!/bin/sh
# run_base58_strict.sh -- the STRICT allocation gate for the base58 byte
# paths: every INTERPOSABLE malloc-family call (libc included) counted
# around each codec region by an allocation interposer
# (tools/alloc-interpose.c). Out of band by construction, and documented as
# such by the out-of-band scoping row below: malloc_zone_malloc/
# malloc_zone_free and mmap never pass through an interposable entry point,
# so a caller using them is invisible here -- that is the interposer's
# stated boundary, not a hole in the claim.
#
# This is the tool-gated companion of tests/run_base58_alloc.sh (which counts
# ENGINE allocations via -T). Together they pin the exact claim:
#
#   base58DecodeInto / base58CheckDecodeInto perform NO allocation beyond
#   their division core's fixed C-heap workspace (one scratch block + one
#   result block per call, both freed before return): zero engine
#   allocations, zero other interposable malloc-family calls of any size,
#   anywhere.
#
# The engine runs with its small-block pools bypassed (DYNAJS_MALLOC_POOLS=0)
# so a POOLED-size temporary (e.g. a JS_NewStringLen("scratchpad", 10) added
# to the decode path) reaches malloc and reddens the rows just like a large
# scratch buffer or a libc-direct strdup would. That is what makes the strict
# zero-alloc claim TESTABLE instead of merely asserted.
#
# Tool-gated: needs a C compiler to build the interposer and a platform that
# supports DYLD_INSERT_LIBRARIES / LD_PRELOAD. Skips LOUDLY without them;
# DYNAJS_REQUIRE_TOOLS=1 turns the skip into a failure.
#
# Usage: tests/run_base58_strict.sh [path-to-dynajs]
set -e
DYNAJS=${1:-./dynajs}
TMP="${TMPDIR:-/tmp}"
OUT="$TMP/base58_strict.$$"
trap 'rm -f "$OUT".trace "$OUT".probe "$OUT".so "$OUT".dylib "$OUT".c "$OUT".zone "$OUT".zone.c "$OUT".zone.probe' EXIT

CC_BIN="${CC:-}"
[ -n "$CC_BIN" ] || CC_BIN=cc
if ! command -v "$CC_BIN" >/dev/null 2>&1; then
    if [ "${DYNAJS_REQUIRE_TOOLS:-0}" = 1 ]; then
        echo "run_base58_strict: REQUIRED tool missing: no C compiler ($CC_BIN)"
        exit 1
    fi
    echo "run_base58_strict: SKIPPED LOUDLY -- no C compiler; the strict"
    echo "  malloc-family half did NOT run (DYNAJS_REQUIRE_TOOLS=1 makes this a failure)"
    exit 0
fi

cp tools/alloc-interpose.c "$OUT.c"
# An ASan-instrumented binary makes this gate MEANINGLESS rather than
# failing: the ASan runtime interposes the malloc family before any
# DYLD_INSERT_LIBRARIES / LD_PRELOAD entry point of ours, so every region
# counts 0/0 while the engine is allocating (measured on the ASan
# test-native leg: the exact-count rows read "0 allocs, want exactly 1228").
# That is the interposer's stated boundary (see the out-of-band scoping row
# below), not a code fact -- report it and skip loudly instead of failing on
# a number the tool cannot see. The exact-count gate itself is untouched on
# native binaries; the engine-count companion run_base58_alloc.sh is
# ASan-safe and keeps running everywhere.
if nm "$DYNAJS" 2>/dev/null | grep -q '_*__asan_'; then
    echo "run_base58_strict: SKIPPED LOUDLY -- $DYNAJS is ASan-instrumented;"
    echo "  the ASan runtime owns the malloc family, so the interposer counts"
    echo "  0/0 on every region by construction (measured) and no exact count"
    echo "  could be enforced or refuted here. The gate runs for real on"
    echo "  native binaries."
    exit 0
fi
if [ "$(uname)" = Darwin ]; then
    "$CC_BIN" -dynamiclib -O1 -o "$OUT.dylib" "$OUT.c" -ldl 2>"$OUT.cc.err" || {
        cat "$OUT.cc.err"
        echo "run_base58_strict: could not build the interposer (tool-gated)"
        [ "${DYNAJS_REQUIRE_TOOLS:-0}" = 1 ] && exit 1
        exit 0
    }
    PRELOAD_ENV="DYLD_INSERT_LIBRARIES=$OUT.dylib"
else
    "$CC_BIN" -shared -fPIC -O1 -o "$OUT.so" "$OUT.c" -ldl 2>"$OUT.cc.err" || {
        cat "$OUT.cc.err"
        echo "run_base58_strict: could not build the interposer (tool-gated)"
        [ "${DYNAJS_REQUIRE_TOOLS:-0}" = 1 ] && exit 1
        exit 0
    }
    PRELOAD_ENV="LD_PRELOAD=$OUT.so"
fi

# the out-of-band scoping probe: one malloc_zone_* (macOS) / mmap (Linux)
# pair inside a counting window. Those entry points are not interposable,
# so the pair MUST count 0/0 -- staying green is the interposer's documented
# boundary, not a bug. Built here (small, self-contained) and run under the
# same interposer right below.
cat > "$OUT.zone.c" <<'ZONEEOF'
#include <stdlib.h>
#ifdef __APPLE__
#include <malloc/malloc.h>
#else
#include <sys/mman.h>
#endif
int main(void)
{
    getenv("##ALLOC-BEGIN out-of-band##");
#ifdef __APPLE__
    {
        void *p = malloc_zone_malloc(malloc_default_zone(), 64);
        malloc_zone_free(malloc_default_zone(), p);
    }
#else
    {
        void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        munmap(p, 4096);
    }
#endif
    getenv("##ALLOC-END##");
    return 0;
}
ZONEEOF
"$CC_BIN" -O1 -o "$OUT.zone" "$OUT.zone.c" 2>"$OUT.cc.err" || {
    cat "$OUT.cc.err"
    echo "run_base58_strict: could not build the out-of-band probe (tool-gated)"
    [ "${DYNAJS_REQUIRE_TOOLS:-0}" = 1 ] && exit 1
    exit 0
}
env "$PRELOAD_ENV" "$OUT.zone" 2> "$OUT.zone.probe" || true

# one run, in-process marker windows; pools OFF so every engine allocation
# is a malloc-family call the interposer sees
env "$PRELOAD_ENV" DYNAJS_MALLOC_POOLS=0 "$DYNAJS" --std tests/test_base58_alloc.js \
    > "$OUT.trace" 2> "$OUT.probe"

count_window() {
    # count_window <window> <awk field: 3=allocs 4=frees>
    awk -v w="$1" -v f="$2" '$1 == "ALLOCPROBE" && $2 == w { print $f }' "$OUT.probe"
}
count_oob() {
    # count_oob <window> <awk field>: the out-of-band scoping probe's output
    awk -v w="$1" -v f="$2" '$1 == "ALLOCPROBE" && $2 == w { print $f }' "$OUT.zone.probe"
}

NLENS=$(sed -n 's/^##SAMPLE lens=\([0-9]*\) .*/\1/p' "$OUT.trace")
NONEMPTY=$(sed -n 's/^##SAMPLE lens=[0-9]* nonempty=\([0-9]*\) .*/\1/p' "$OUT.trace")
if [ -z "$NLENS" ] || [ -z "$NONEMPTY" ]; then
    echo "run_base58_strict: no ##SAMPLE line in the workload output"
    exit 1
fi

fail=0
check() {
    # check <window> <field> <expected> <note>
    got=$(count_window "$1" "$2")
    if [ -z "$got" ]; then
        echo "FAIL $1: no ALLOCPROBE line (the interposer did not see the window)"
        fail=1
        return
    fi
    if [ "$got" -eq "$3" ]; then
        echo "ok   $1: $got $4 ($5)"
    else
        echo "FAIL $1: $got $4, want exactly $3 ($5)"
        fail=1
    fi
}

echo "info sweep: $NLENS lengths ($NONEMPTY non-empty), pool zone 0..512 dense"
echo "info counting mode: STRICT malloc-family (engine pools bypassed; interposer over $(uname))"

# sanity: the engine must have honored the pool bypass (32 tiny encoder calls
# = 32 result strings and their workspaces; 96 malloc-family calls on an
# eager-string engine, 64 where result strings materialize lazily; 0 would
# mean the pools stayed on and a pooled temporary could hide again)
probe=$(count_window pool-probe 3)
if [ -z "$probe" ] || [ "$probe" -lt 32 ]; then
    echo "FAIL pool-probe: $probe allocations -- the engine did not bypass its pools;"
    echo "     this harness must see every interposable engine allocation (DYNAJS_MALLOC_POOLS=0)"
    fail=1
else
    echo "ok   pool-probe: $probe allocations (pools bypassed, every interposable engine allocation visible)"
fi

# ---- the interposer's boundary, documented by a scoping row: one
# out-of-band pair (malloc_zone_* on macOS, mmap on Linux) sits inside a
# counting window and must show 0/0. Those entry points never pass through
# an interposable symbol, so staying GREEN is the documented scope of
# "every interposable malloc-family call" -- the boundary itself, not a
# missed allocation or a bug in the row.
oob_a=$(count_oob out-of-band 3)
oob_f=$(count_oob out-of-band 4)
if [ -z "$oob_a" ] || [ -z "$oob_f" ]; then
    echo "FAIL out-of-band: no ALLOCPROBE line (the scoping probe did not run)"
    fail=1
elif [ "$oob_a" -eq 0 ] && [ "$oob_f" -eq 0 ]; then
    echo "ok   out-of-band: 0 allocs / 0 frees (malloc_zone_*/mmap stay green -- the documented boundary of the interposable set, not a bug)"
else
    echo "FAIL out-of-band: $oob_a allocs / $oob_f frees, want 0/0 (the zone/mmap pair must stay invisible here)"
    fail=1
fi

# ---- THE strict claim: the decode-into byte path allocates nothing beyond
# the division workspace: one scratch calloc + one result malloc per call,
# both freed before return -> exactly 2*N allocations and 2*N frees.

EACH=$((2 * NLENS))
for w in decode-into-ones decode-into-zs decode-into-mixed; do
    check "$w" 3 "$EACH" "allocs" "2/call division workspace ($NLENS calls), nothing else"
    check "$w" 4 "$EACH" "frees" "the workspace is released before return (no retention)"
done
check check-decode-into 3 "$((2 * 3 * 65))" "allocs" "2/call division workspace (3x65 calls), nothing else"
check check-decode-into 4 "$((2 * 3 * 65))" "frees" "the workspace is released before return (no retention)"

# ---- the one-shot text encoders on the C heap: BINARY-RESTATING rows via
# the in-run calibration twins. The result strings' cost is an ENGINE STRING
# POLICY (one eager allocation per fresh string here, deferred lazy
# materialization on a lazy-string engine), not the codec's traffic, so no
# absolute count can pin these rows across engines. Each encoder window's
# cal-* twin builds one fresh string of the SAME result length per call in
# the same run -- the twin restates the string policy exactly, and the
# DIFFERENCE encode-minus-twin must be exactly the codec's fixed C-heap
# workspace: 2 calls per call (scratch + result block) for the plain
# encoders, 3 for check-encode (plus its checksum payload block). Once the
# policy-independent workspace is accounted for, the difference is ZERO --
# any temporary, scratch buffer or libc-direct allocation added to an
# encode path reddens its row on ANY engine.
check_shape() {
    # check_shape <window> <twin> <workspace-per-call> <calls> <note>
    got=$(count_window "$1" 3)
    twin=$(count_window "$2" 3)
    if [ -z "$got" ] || [ -z "$twin" ]; then
        echo "FAIL $1: no ALLOCPROBE line (the interposer did not see the window)"
        fail=1
        return
    fi
    ws=$(( $3 * $4 ))
    d=$((got - twin - ws))
    if [ "$d" -eq 0 ]; then
        echo "ok   $1: $got allocs - $twin twin - $ws workspace = 0 ($5)"
    else
        echo "FAIL $1: $got allocs - $twin twin - $ws workspace = $d, want 0 ($5)"
        fail=1
    fi
}

check_shape encode-zeros cal-zeros 2 "$NLENS" "workspace-only overhead over the twin's result strings, nothing else"
check_shape encode-ffs   cal-ffs   2 "$NLENS" "workspace-only overhead over the twin's result strings, nothing else"
check_shape encode-alt   cal-alt   2 "$NLENS" "workspace-only overhead over the twin's result strings, nothing else"
check_shape encode-mixed cal-mixed 2 "$NLENS" "workspace-only overhead over the twin's result strings, nothing else"
check_shape basex-encode cal-basex 2 "$NLENS" "workspace-only overhead over the twin's result strings, nothing else"
check_shape check-encode cal-check 3 "$NLENS" "checksum payload + workspace over the twin's result strings, nothing else"

# ---- retention (the free-side claim is policy-independent in shape, not in
# count): the region releases everything it allocated before returning --
# frees == allocs inside the window. The ABSOLUTE free count restates with
# the string policy and is deliberately not pinned.
enc_a=$(count_window encode-zeros 3)
enc_f=$(count_window encode-zeros 4)
if [ -z "$enc_a" ] || [ -z "$enc_f" ]; then
    echo "FAIL encode-zeros frees: no ALLOCPROBE line"
    fail=1
elif [ "$enc_a" -eq "$enc_f" ]; then
    echo "ok   encode-zeros: $enc_f frees == $enc_a allocs (the region retains nothing)"
else
    echo "FAIL encode-zeros: $enc_f frees != $enc_a allocs (retention on the encode path)"
    fail=1
fi

# report-only windows (the lazy-string sweep and warmup): printed for the
# record, asserted nothing -- lazy (sliced) argument strings materialize in
# the engine's string machinery, which is an engine string policy, not the
# codec's traffic
for w in decode-into-sliced-info warmup; do
    echo "info $w: $(count_window "$w" 3) allocs / $(count_window "$w" 4) frees (report-only)"
done

if [ "$fail" -ne 0 ]; then
    echo "run_base58_strict: FAILURES (see above)"
    exit 1
fi
echo "run_base58_strict: all malloc-family counts exact (strict zero-alloc claim proven)"
