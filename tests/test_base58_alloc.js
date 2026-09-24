// flags: --std
/* test_base58_alloc.js -- the allocation-COUNT workload for the base58 byte
 * paths. Two harnesses drive it:
 *
 *   tests/run_base58_alloc.sh   (ALWAYS-ON gate) runs the engine with -T (one
 *       "A <size>" line per ENGINE allocation) and counts allocations between
 *       the ##ALLOC-BEGIN <name>## / ##ALLOC-END## markers printed here.
 *   tests/run_base58_strict.sh  (tool-gated) runs it under a malloc-family
 *       interposer that counts every INTERPOSABLE malloc-family call on the
 *       C heap -- the engine's and libc's alike -- between the same markers.
 *       Out of band by construction (the interposer's documented boundary,
 *       not a gap in the claim): malloc_zone_malloc/malloc_zone_free and
 *       mmap never pass through an interposable entry point; the strict
 *       harness carries a scoping row that documents this boundary.
 *
 * THE CLAIMS UNDER TEST, stated exactly:
 *
 *   1. base58DecodeInto / base58CheckDecodeInto touch the ENGINE allocator
 *      not at all: 0 engine allocations per call. (The division core keeps
 *      its own workspace on the C heap -- two malloc-family calls per call,
 *      scratch + result block, both freed before return; the strict harness
 *      pins that count exactly, so ANY extra traffic -- a temporary, a
 *      scratch buffer, a libc-direct allocation -- reddens a row.)
 *   2. The one-shot text encoders cost exactly ONE ENGINE allocation per
 *      non-empty call -- the result string -- independent of length and
 *      content (the empty result is the interned empty string). Their C-heap
 *      scratch (the quadratic division workspace) is fixed per call and is
 *      pinned by the strict harness.
 *
 * INPUT SWEEP: every length 0..512 (the arena/pool size zone, where the
 * pooled/unpooled split lives), then every 37th length up to the 4096 cap
 * plus the 1024/2048/3072/4095/4096 boundary lengths. (The codecs are
 * O(n^2) division loops; an every-length sweep to 4096 costs minutes per
 * region and buys no coverage the dense pool zone plus the stride misses.)
 * Each region's call accounting is printed as a ##SAMPLE line before the
 * regions so the harness asserts absolute counts, not guesses.
 *
 * POOLED REALITY (why this workload has two counting modes): the engine's
 * small-block arenas serve allocations up to ~512 bytes from free-lists that
 * never reach js_malloc, so on a pooled build an "A" line only appears for
 * LARGE/unpooled allocations. run_base58_alloc.sh therefore runs this
 * workload with DYNAJS_MALLOC_POOLS=0 (every allocation reaches js_malloc,
 * claims 1-2 asserted in full); on an older binary that ignores that switch
 * it falls back to the pooled-aware restatement below, and the pool-probe
 * region tells the two apart. The fallback rows assert exactly what IS
 * measurable there: zero large/unpooled allocations on the decode-into byte
 * path, and for the encoders "one result-string-shaped allocation per call
 * and nothing else" (each encode row must equal its cal-* twin, which builds
 * one fresh string of the SAME result length per call).
 *
 * All fixtures (texts, keys, byte buffers, result lengths) are built BEFORE
 * each region's BEGIN marker: the measurement window must contain nothing
 * but the codec calls and their loop. */
import { Base58Encode, Base58CheckEncode, base58DecodeInto,
         base58CheckDecodeInto, BaseXEncode } from "dyna:encoding";
import * as std from "std";

const MAX = 4096;
const HEX = "0123456789abcdef";

/* the input-length sweep (see the header) */
const LENS = [];
for (let len = 0; len <= 512; len++) LENS.push(len);
for (let len = 549; len <= MAX; len += 37) LENS.push(len);
for (const b of [1024, 2048, 3072, 4095, 4096])
    if (LENS.indexOf(b) < 0) LENS.push(b);
LENS.sort((a, b) => a - b);
let nNonempty = 0;
for (const len of LENS) if (len > 0) nNonempty++;

/* ---- fixtures (outside every measurement region) ----------------------
 * Every per-length string and view is built HERE: slicing or subarray-ing
 * inside a region would allocate and contaminate the measurement.
 * Strings must be FLAT (repeat/padEnd-built): a sliced (lazy, parent-backed)
 * string materializes its bytes when converted for the codec -- an engine
 * string-machinery allocation that is NOT the byte path's (the report-only
 * region at the bottom measures it for contrast). */
const big = new Uint8Array(MAX + 1);
const bigZeros = new Uint8Array(MAX + 1);                 /* all-zero input */
const bigFfs = new Uint8Array(MAX + 1).fill(0xff);        /* carry-storm    */
const bigAlt = new Uint8Array(MAX + 1);                   /* alternating    */
for (let i = 0; i <= MAX; i++) { big[i] = (i * 37 + 11) & 0xff; bigAlt[i] = (i & 1) ? 0xff : 0x01; }
const out = new Uint8Array(MAX + 64);

/* decode-into texts at every swept length: all-'1' (all-zero payload),
   all-'z' (maximal carries) and mixed. '1' and 'z' are both legal base58. */
const textsOnes = [], textsZs = [], textsMixed = [], textsSliced = [];
const srcSliced = "z".repeat(MAX);
const viewsZeros = [], viewsFfs = [], viewsAlt = [], viewsMixed = [];
for (const len of LENS) {
    textsOnes.push("1".repeat(len));
    textsZs.push("z".repeat(len));
    textsMixed.push("S".padEnd(len, "tV1DL6CwTryKy"));   /* flat, varied */
    textsSliced.push(srcSliced.slice(0, len));           /* lazy strings  */
    viewsZeros.push(bigZeros.subarray(0, len));
    viewsFfs.push(bigFfs.subarray(0, len));
    viewsAlt.push(bigAlt.subarray(0, len));
    viewsMixed.push(big.subarray(0, len));
}
/* Base58Check texts must carry a valid checksum to reach the byte path;
   pre-built outside the region. */
const checkTexts = [];
for (let len = 0; len <= 64; len++)
    checkTexts.push(Base58CheckEncode(big.slice(0, len)));

/* cal-* fixtures: the RESULT LENGTH of every encoder call above, then a
   prebuilt unit string each cal call grows to a plain string of exactly
   that length -- one fresh string per call, the same result-string shape
   the encoder's return value has under any engine string policy (eager or
   lazy), so the twin row restates the encoder row's string cost in-run. */
const lensZeros = [], lensFfs = [], lensAlt = [], lensMixed = [],
      lensCheck = [], lensBasex = [];
for (let i = 0; i < LENS.length; i++) {
    lensZeros.push(Base58Encode(viewsZeros[i]).length);
    lensFfs.push(Base58Encode(viewsFfs[i]).length);
    lensAlt.push(Base58Encode(viewsAlt[i]).length);
    lensMixed.push(Base58Encode(viewsMixed[i]).length);
    lensCheck.push(Base58CheckEncode(viewsMixed[i]).length);
    lensBasex.push(BaseXEncode(viewsMixed[i], HEX).length);
}
const calUnit = "x";
const calTwo = "xy";   /* slice source for fresh 1-char twins (see below) */

/* the pool probe: 32 encoder calls whose results are tiny (well under the
   pool's 512-byte ceiling). Pooled build: 0 "A" lines (free-list served).
   Pools-off build: exactly 32 (one result string per call). */
const probeView = bigZeros.subarray(0, 8);

function region(mark, fn) {
    /* the getenv calls are the marker door for the malloc interposer in
       tests/run_base58_strict.sh (macOS stdio flushes through
       write$NOCANCEL, so the printed marker alone cannot delimit a window);
       the prints delimit the -T trace for tests/run_base58_alloc.sh */
    std.getenv(mark);
    print(mark);
    std.out.flush();
    fn();
    std.getenv(MEND);
    print(MEND);
    std.out.flush();
}
/* marker strings are PREBUILT (outside every region): a concatenation in
   the marker path would allocate inside the measurement window */
const MEND = "##ALLOC-END##";
const M = {
    warm: "##ALLOC-BEGIN warmup##",
    base: "##ALLOC-BEGIN baseline##",
    ones: "##ALLOC-BEGIN decode-into-ones##",
    zs: "##ALLOC-BEGIN decode-into-zs##",
    mixed: "##ALLOC-BEGIN decode-into-mixed##",
    chk: "##ALLOC-BEGIN check-decode-into##",
    ez: "##ALLOC-BEGIN encode-zeros##",
    ef: "##ALLOC-BEGIN encode-ffs##",
    ea: "##ALLOC-BEGIN encode-alt##",
    em: "##ALLOC-BEGIN encode-mixed##",
    ce: "##ALLOC-BEGIN check-encode##",
    bx: "##ALLOC-BEGIN basex-encode##",
    kz: "##ALLOC-BEGIN cal-zeros##",
    kf: "##ALLOC-BEGIN cal-ffs##",
    ka: "##ALLOC-BEGIN cal-alt##",
    km: "##ALLOC-BEGIN cal-mixed##",
    kc: "##ALLOC-BEGIN cal-check##",
    kx: "##ALLOC-BEGIN cal-basex##",
    pp: "##ALLOC-BEGIN pool-probe##",
};

/* The harness reads the call accounting from this line. */
print("##SAMPLE lens=" + LENS.length + " nonempty=" + nNonempty +
      " checkcalls=" + checkTexts.length + " probe=32##");
std.out.flush();

/* Warmup: absorbs the one-time costs of the print/flush machinery (stdio
   buffer, atom interning) so every later region's artifact is EXACTLY zero
   and raw counts are the assertion. */
region(M.warm, () => {
    let x = 0;
    for (let i = 0; i <= 8; i++) x += i;
    if (x < 0) print("never");
});

/* The control: the sweep loops alone allocate nothing. */
region(M.base, () => {
    let x = 0;
    for (let i = 0; i < LENS.length; i++) x += LENS[i];
    for (let i = 0; i <= 64; i++) x += i;
    for (let k = 0; k < 3; k++) x += k;
    if (x < 0) print("never");
});

/* THE zero-alloc claim: decode-into at every swept length, three input
   classes (all-zero, carry-storm, mixed) -- zero ENGINE allocations. */
region(M.ones, () => {
    for (let i = 0; i < LENS.length; i++)
        base58DecodeInto(textsOnes[i], out);
});
region(M.zs, () => {
    for (let i = 0; i < LENS.length; i++)
        base58DecodeInto(textsZs[i], out);
});
region(M.mixed, () => {
    for (let i = 0; i < LENS.length; i++)
        base58DecodeInto(textsMixed[i], out);
});
region(M.chk, () => {
    for (let k = 0; k < 3; k++)
        for (let i = 0; i < checkTexts.length; i++)
            base58CheckDecodeInto(checkTexts[i], out);
});

/* The text encoders: exactly ONE engine allocation per non-empty call (the
   result string), independent of length and content. (Length 0 encodes to
   the canonical empty string, which allocates nothing.) */
region(M.ez, () => {
    for (let i = 0; i < LENS.length; i++)
        Base58Encode(viewsZeros[i]);
});
region(M.ef, () => {
    for (let i = 0; i < LENS.length; i++)
        Base58Encode(viewsFfs[i]);
});
region(M.ea, () => {
    for (let i = 0; i < LENS.length; i++)
        Base58Encode(viewsAlt[i]);
});
region(M.em, () => {
    for (let i = 0; i < LENS.length; i++)
        Base58Encode(viewsMixed[i]);
});
region(M.ce, () => {
    for (let i = 0; i < LENS.length; i++)
        Base58CheckEncode(viewsMixed[i]);
});
region(M.bx, () => {
    for (let i = 0; i < LENS.length; i++)
        BaseXEncode(viewsMixed[i], HEX);
});

/* cal-* twins: one fresh result-shaped string per encoder call of the same
   name, at exactly that call's result length -- the in-run mirror of the
   encoder's result-string cost under ANY engine string policy (an eager
   engine pays one allocation per fresh string, a lazy one defers them in
   both rows identically). Two edges follow the encoder's documented result
   semantics rather than repeat()'s own fast paths: the empty result is the
   canonical interned empty string (zero cost -- no twin call), and
   repeat(1) hands its input back for free where the encoder's 1-char
   result is a fresh string (the twin builds that string with a slice).
   With the mirror exact, an encoder row MINUS its twin is the codec's
   fixed C-heap workspace and nothing else -- that is the policy-
   independent difference the strict harness asserts. */
region(M.kz, () => {
    for (let i = 0; i < LENS.length; i++) {
        const n = lensZeros[i];
        if (n === 1) calTwo.slice(0, 1);
        else if (n > 1) calUnit.repeat(n);
    }
});
region(M.kf, () => {
    for (let i = 0; i < LENS.length; i++) {
        const n = lensFfs[i];
        if (n === 1) calTwo.slice(0, 1);
        else if (n > 1) calUnit.repeat(n);
    }
});
region(M.ka, () => {
    for (let i = 0; i < LENS.length; i++) {
        const n = lensAlt[i];
        if (n === 1) calTwo.slice(0, 1);
        else if (n > 1) calUnit.repeat(n);
    }
});
region(M.km, () => {
    for (let i = 0; i < LENS.length; i++) {
        const n = lensMixed[i];
        if (n === 1) calTwo.slice(0, 1);
        else if (n > 1) calUnit.repeat(n);
    }
});
region(M.kc, () => {
    for (let i = 0; i < LENS.length; i++) {
        const n = lensCheck[i];
        if (n === 1) calTwo.slice(0, 1);
        else if (n > 1) calUnit.repeat(n);
    }
});
region(M.kx, () => {
    for (let i = 0; i < LENS.length; i++) {
        const n = lensBasex[i];
        if (n === 1) calTwo.slice(0, 1);
        else if (n > 1) calUnit.repeat(n);
    }
});

/* the pooling probe (see the header): 32 tiny encoder calls */
region(M.pp, () => {
    for (let k = 0; k < 32; k++)
        Base58Encode(probeView);
});

/* Report-only: the same sweep with LAZY (sliced) argument strings. Those
   materialize their bytes at conversion time in the engine's string
   machinery -- before any codec byte runs -- so this region is expected to
   show engine allocations and asserts nothing. It exists to keep the
   flat-string regions honest: if the zero counts above were an artifact of
   the counting, this region would show 0 too. */
region("##ALLOC-BEGIN decode-into-sliced-info##", () => {
    for (let i = 0; i < LENS.length; i++)
        base58DecodeInto(textsSliced[i], out);
});

print("test_base58_alloc: workload complete (" + LENS.length + " lengths, " +
      nNonempty + " non-empty, pool zone 0..512 dense)");
std.out.flush();
