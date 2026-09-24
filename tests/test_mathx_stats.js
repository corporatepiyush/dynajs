/* test_mathx_stats.js — dyna:mathx `stats` + `bits.rotateRight*`.
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_mathx_stats.js
 * Prints "test_mathx_stats: all tests passed" on success; throws on failure.
 *
 * Oracle policy (loop-2 mandate): every stats function is pinned against
 * values computed in Python (math.fsum / two-pass / exact R-7 quantile) and
 * differentially tested against naive scalar JS references on deterministic
 * pseudo-random data. The Float64Array fast path and the number[] fallback
 * must agree BIT-FOR-BIT on identical values (they run the identical
 * sequential Neumaier fold); that is asserted everywhere, not assumed.
 */

import { stats, bits } from "dyna:mathx";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}
function assertEq(actual, expected, msg) {
    n++;
    if (!(actual === expected) && !Object.is(actual, expected))
        throw new Error("assertion failed: " + msg +
            " (got " + actual + ", expected " + expected + ")");
}
function assertNaN(actual, msg) {
    n++;
    if (!Number.isNaN(actual))
        throw new Error("assertion failed: " + msg + " (got " + actual + ", expected NaN)");
}
function assertThrows(fn, name, msg) {
    n++;
    try {
        fn();
    } catch (e) {
        if (name && e.name !== name)
            throw new Error("assertion failed: " + msg + " (threw " +
                e.name + ": " + e.message + ", expected " + name + ")");
        return;
    }
    throw new Error("assertion failed: " + msg + " (threw nothing, expected " + name + ")");
}
/* Same value bit-for-bit across the two input paths. */
function assertSameBits(a, b, msg) {
    n++;
    if (!Object.is(a, b) && !(typeof a === "number" && typeof b === "number" &&
                              Number.isNaN(a) && Number.isNaN(b)))
        throw new Error("assertion failed: " + msg +
            " (Float64Array path " + a + " vs number[] path " + b + ")");
}
/* Relative closeness, for C-vs-JS differentials where the C kernel's FMA
 * contraction legitimately single-rounds an expression the JS reference
 * double-rounds (a documented <=1-ulp effect, the same one DataFrame's
 * kernels warn about). */
function assertCloseRel(actual, expected, rel, msg) {
    n++;
    const scale = Math.max(1, Math.abs(expected));
    if (!(Math.abs(actual - expected) <= rel * scale))
        throw new Error("assertion failed: " + msg +
            " (got " + actual + ", expected ~" + expected + " rel " + rel + ")");
}

/* Deterministic LCG so failures reproduce. */
function lcg(seed) {
    let s = seed >>> 0;
    return function () {
        s = (Math.imul(s, 1664525) + 1013904223) >>> 0;
        return s / 4294967296;
    };
}

/* ---------- naive scalar references (the oracle's dumb twin) ---------- */
function naiveSum(a) {
    let s = 0;
    for (let i = 0; i < a.length; i++) s += a[i];
    return s;
}
function naiveMean(a) { return naiveSum(a) / a.length; }
function naiveVar(a, pop) {
    const m = naiveMean(a);
    let m2 = 0;
    for (let i = 0; i < a.length; i++) m2 += (a[i] - m) * (a[i] - m);
    return m2 / (pop ? a.length : a.length - 1);
}
function naiveCov(x, y, pop) {
    const mx = naiveMean(x), my = naiveMean(y);
    let c = 0;
    for (let i = 0; i < x.length; i++) c += (x[i] - mx) * (y[i] - my);
    return c / (pop ? x.length : x.length - 1);
}
function naiveCorr(x, y) {
    const mx = naiveMean(x), my = naiveMean(y);
    let c = 0, vx = 0, vy = 0;
    for (let i = 0; i < x.length; i++) {
        const dx = x[i] - mx, dy = y[i] - my;
        c += dx * dy; vx += dx * dx; vy += dy * dy;
    }
    return c / (Math.sqrt(vx) * Math.sqrt(vy));
}
/* The R-7 / "linear" quantile, straight from the definition. */
function naiveQuantile(a, q) {
    const b = a.slice().sort(function (x, y) { return x - y; });
    const pos = q * (b.length - 1);
    const lo = Math.floor(pos);
    const frac = pos - lo;
    if (frac === 0 || lo + 1 >= b.length) return b[lo];
    return b[lo] + frac * (b[lo + 1] - b[lo]);
}

/* The fixed fixture, pinned against Python (math.fsum / exact two-pass). */
const DATA = [3.5, -2.25, 17.125, 0.0, 4.5, -2.25, 9.75, 3.5, -8.125, 6.0, 2.125, 12.375];
const DATA_F64 = new Float64Array(DATA);
const X2 = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12];

/* ================================================================ *
 *  sum: pinned oracle values + beats naive
 * ================================================================ */
{
    assertEq(stats.sum(DATA), 46.25, "sum of fixture");
    assertEq(stats.sum(new Float64Array([0.1, 0.2])), 0.1 + 0.2, "sum [0.1,0.2]");
    /* Python: math.fsum([0.1]*100000) == 10000.0 exactly; the naive
     * accumulator lands on 10000.000000018848. */
    const many = new Float64Array(100000).fill(0.1);
    assertEq(stats.sum(many), 10000.0, "sum 1e5 x 0.1 is exact");
    assert(Math.abs(stats.sum(many) - 10000.0) < Math.abs(naiveSum(many) - 10000.0),
           "Neumaier beats naive on 1e5 x 0.1");
    /* Python: fsum([1e16,1,-1e16]*1000) == 1000.0; naive drifts to 0.0. */
    const canc = [];
    for (let i = 0; i < 1000; i++) canc.push(1e16, 1, -1e16);
    assertEq(stats.sum(canc), 1000, "compensated cancellation recovers the 1s");
    assertEq(stats.sum(new Float64Array(canc)), 1000, "same, fast path");
    assertEq(naiveSum(canc), 0, "oracle sanity: naive really does lose them");
    /* Python neumaier trace of the overflow: s overflows to Inf, the
     * compensation term becomes Inf-Inf = NaN, total is NaN. */
    assertNaN(stats.sum([1e308, 1e308, 1e308, -3e308]), "overflowed compensation is NaN");
    assertEq(stats.sum([]), 0, "sum of empty is +0");
    assertEq(stats.sum([-0]), 0, "sum absorbs -0 into the +0 accumulator");
}

/* ================================================================ *
 *  mean / variance / stddev: pinned + differential
 * ================================================================ */
{
    assertEq(stats.mean(DATA), 3.8541666666666665, "mean of fixture");
    assertEq(stats.variance(DATA), 47.69270833333333, "sample variance of fixture");
    assertEq(stats.variance(DATA, { pop: true }), 43.71831597222222, "population variance");
    assertEq(stats.stddev(DATA), 6.905990756823623, "sample stddev");
    assertEq(stats.stddev(DATA, { pop: true }), Math.sqrt(43.71831597222222),
             "population stddev is sqrt of population variance");

    /* empty / single-element gates */
    assertNaN(stats.mean([]), "mean of empty is NaN");
    assertNaN(stats.variance([]), "sample variance of empty is NaN");
    assertNaN(stats.variance([], { pop: true }), "population variance of empty is NaN");
    assertNaN(stats.variance([7]), "sample variance of one point is NaN");
    assertEq(stats.variance([7], { pop: true }), 0, "population variance of one point is 0");
    assertNaN(stats.stddev([]), "stddev of empty is NaN");
    assertEq(stats.variance([3, 3, 3]), 0, "constant data, sample variance 0");
    assertEq(stats.variance([3, 3, 3], { pop: true }), 0, "constant data, population variance 0");

    /* opts handling */
    assertEq(stats.variance(DATA, undefined), stats.variance(DATA), "undefined opts = sample");
    assertEq(stats.variance(DATA, null), stats.variance(DATA), "null opts = sample");
    assertEq(stats.variance(DATA, {}), stats.variance(DATA), "empty opts = sample");
    assertEq(stats.variance(DATA, { pop: 1 }), stats.variance(DATA, { pop: true }),
             "pop coerces like JS truthiness");
    assertThrows(function () { stats.variance(DATA, 42); }, "TypeError",
                 "non-object opts is refused");

    /* the pop/sample ratio is exact: one m2, two divisors */
    assertEq(stats.variance(DATA) * 11, stats.variance(DATA, { pop: true }) * 12,
             "var_s * (n-1) == var_p * n exactly");

    /* differential vs the naive two-pass on deterministic random data */
    const rand = lcg(42);
    for (let t = 0; t < 20; t++) {
        const len = 1 + Math.floor(rand() * 200);
        const a = [];
        for (let i = 0; i < len; i++) a.push((rand() - 0.5) * 1e6);
        const af = new Float64Array(a);
        assertSameBits(stats.mean(af), stats.mean(a), "mean paths agree n=" + len);
        assertSameBits(stats.variance(af), stats.variance(a), "variance paths agree n=" + len);
        assert(Math.abs(stats.mean(a) - naiveMean(a)) <= 1e-12 * Math.max(1, Math.abs(naiveMean(a))),
               "mean ~ naive n=" + len);
        assert(Math.abs(stats.variance(a) - naiveVar(a, false)) <=
               1e-10 * Math.max(1, naiveVar(a, false)), "variance ~ naive n=" + len);
    }
}

/* ================================================================ *
 *  median / quantile: convention + differential + validation
 * ================================================================ */
{
    /* pinned against Python's R-7 on the fixture */
    assertEq(stats.median(DATA), 3.5, "median of fixture");
    assertEq(stats.quantile(DATA, 0.0), -8.125, "q=0 is the min");
    assertEq(stats.quantile(DATA, 0.1), -2.25, "q=0.1");
    assertEq(stats.quantile(DATA, 0.25), -0.5625, "q=0.25 interpolates");
    assertEq(stats.quantile(DATA, 0.5), 3.5, "q=0.5 == median");
    assertEq(stats.quantile(DATA, 0.75), 6.9375, "q=0.75");
    assertEq(stats.quantile(DATA, 0.9), 12.1125, "q=0.9");
    assertEq(stats.quantile(DATA, 1.0), 17.125, "q=1 is the max");
    assertEq(stats.median([1, 2, 3, 4]), 2.5, "median of even count");
    assertEq(stats.median([7]), 7, "median of a single point");
    assertEq(stats.quantile([7], 0.3), 7, "quantile of a single point");

    /* differential vs the definition on random data, both paths */
    const rand = lcg(7);
    for (let t = 0; t < 20; t++) {
        const len = 1 + Math.floor(rand() * 40);
        const a = [];
        for (let i = 0; i < len; i++) a.push(Math.floor(rand() * 100) - 50);
        const af = new Float64Array(a);
        for (const q of [0, 0.05, 0.25, 0.4, 0.5, 0.6, 0.75, 0.95, 1]) {
            assertCloseRel(stats.quantile(a, q), naiveQuantile(a, q), 1e-14,
                           "quantile ~ R-7 n=" + len + " q=" + q);
            assertSameBits(stats.quantile(af, q), stats.quantile(a, q),
                           "quantile paths agree n=" + len + " q=" + q);
        }
        assertSameBits(stats.median(af), stats.median(a), "median paths agree n=" + len);
        assertEq(stats.median(a), naiveQuantile(a, 0.5), "median ~ R-7 n=" + len);
    }

    /* REVIEW FINDING (claim accuracy): for INTERPOLATED quantiles the C
     * kernel's FMA contraction can single-round where the exact R-7
     * reference double-rounds — a 1-ULP class divergence on adversarial
     * magnitudes (the pinned LCG fixtures above happen to be exact).
     * Contract: 1-ULP agreement for interpolated q; EXACT for frac==0
     * ranks (median below is bit-exact). */
    {
        const big = [];
        for (let i = 0; i < 1000; i++) big.push(1e16, 1, -1e16);
        const q = 1 / 3;
        assertCloseRel(stats.quantile(big, q), naiveQuantile(big, q), 1e-15,
                       "interpolated quantile ~ exact R-7 within the FMA 1-ULP class");
        assertEq(stats.median(big), naiveQuantile(big, 0.5),
                 "frac==0 ranks are bit-exact (no contraction window)");
    }

    /* validation */
    assertThrows(function () { stats.median([]); }, "RangeError", "median of empty");
    assertThrows(function () { stats.quantile([], 0.5); }, "RangeError", "quantile of empty");
    assertThrows(function () { stats.quantile(DATA, -0.1); }, "RangeError", "q < 0");
    assertThrows(function () { stats.quantile(DATA, 1.0001); }, "RangeError", "q > 1");
    assertThrows(function () { stats.quantile(DATA, NaN); }, "RangeError", "q = NaN");
    /* NaN input: order statistics are undefined, the answer is NaN */
    assertNaN(stats.median([3, NaN, 1]), "median of NaN-bearing data");
    assertNaN(stats.quantile([3, NaN, 1], 0.25), "quantile of NaN-bearing data");
    /* infinities sort normally... */
    assertEq(stats.quantile([0, Infinity], 0.25), Infinity, "interpolating toward Inf");
    /* ...and the 0*(Inf-Inf) guard matches Python's R-7 trace: NaN */
    assertNaN(stats.quantile([1, Infinity, Infinity], 0.75),
              "interpolating between two Infinities is NaN (frac!=0 arm)");
    assertEq(stats.quantile([1, Infinity, Infinity], 0.5), Infinity,
             "exact rank (frac==0 arm) reads the order statistic, no interpolation");
}

/* ================================================================ *
 *  min / max: Math.min semantics
 * ================================================================ */
{
    assertEq(stats.min(DATA), -8.125, "min of fixture");
    assertEq(stats.max(DATA), 17.125, "max of fixture");
    assertEq(stats.min([]), Infinity, "min of empty is the Math.min() identity");
    assertEq(stats.max([]), -Infinity, "max of empty is the Math.max() identity");
    assertNaN(stats.min([1, NaN, 2]), "NaN poisons min");
    assertNaN(stats.max([1, NaN, 2]), "NaN poisons max");
    assertNaN(stats.min(new Float64Array([1, NaN, 2])), "NaN poisons min (fast path)");
    /* signed zeros: Math.min keeps -0, Math.max keeps +0 */
    assert(Object.is(stats.min([0, -0]), -0), "min([0,-0]) is -0");
    assert(Object.is(stats.max([-0, 0]), 0), "max([-0,0]) is +0");
    assert(Object.is(stats.min([-0]), -0), "min([-0]) is -0");
    assert(Object.is(stats.min([-3, -0, -2]), -3), "min ignores zeros when negatives exist");
    /* differential vs Math.min/Math.max on random data */
    const rand = lcg(11);
    for (let t = 0; t < 10; t++) {
        const len = 1 + Math.floor(rand() * 100);
        const a = [];
        for (let i = 0; i < len; i++) a.push(Math.floor(rand() * 11) - 5); /* includes +/-0 */
        const af = new Float64Array(a);
        assertSameBits(stats.min(af), stats.min(a), "min paths agree n=" + len);
        assertSameBits(stats.max(af), stats.max(a), "max paths agree n=" + len);
        let mn = Infinity, mx = -Infinity, poison = false;
        for (const x of a) {
            if (Number.isNaN(x)) poison = true;
            mn = Math.min(mn, x);
            mx = Math.max(mx, x);
        }
        if (poison) {
            assertNaN(stats.min(a), "min NaN differential");
        } else {
            assert(Object.is(stats.min(a), mn), "min ~ Math.min n=" + len);
            assert(Object.is(stats.max(a), mx), "max ~ Math.max n=" + len);
        }
    }
}

/* ================================================================ *
 *  cov / corr: pinned, differential, mismatch and degeneracy
 * ================================================================ */
{
    /* pinned against Python on the fixture vs 1..12 */
    assertEq(stats.cov(DATA, X2), 1.2499999999999998, "sample covariance");
    assertEq(stats.cov(DATA, X2, { pop: true }), 1.145833333333333, "population covariance");
    assertEq(stats.corr(DATA, X2), 0.05020099719916587, "Pearson r");
    assertCloseRel(stats.cov(DATA, X2) * 11, stats.cov(DATA, X2, { pop: true }) * 12,
                   1e-14, "cov_s * (n-1) == cov_p * n (to ulps; two roundings)");

    /* perfect (positive/negative) correlation, clamped inside [-1, 1] */
    const r1 = stats.corr([1, 2, 3], [2, 4, 6]);
    assert(r1 <= 1 && r1 > 0.999999, "perfect positive correlation, not above 1");
    assert(stats.corr([1, 2, 3], [-2, -4, -6]) < -0.999999, "perfect negative correlation");

    /* degeneracy */
    assertThrows(function () { stats.corr([1, 1, 1], [1, 2, 3]); }, "RangeError",
                 "zero variance throws");
    assertThrows(function () { stats.corr([], []); }, "RangeError", "corr of empty throws");
    assertThrows(function () { stats.cov([1, 2], [1, 2, 3]); }, "RangeError",
                 "cov length mismatch");
    assertThrows(function () { stats.corr([1, 2], [1, 2, 3]); }, "RangeError",
                 "corr length mismatch");
    assertNaN(stats.cov([1], [2]), "sample cov of one point is NaN");
    assertEq(stats.cov([1], [2], { pop: true }), 0, "population cov of one point is 0");
    assertNaN(stats.cov([NaN, 1], [1, 2]), "NaN poisons cov");
    assertNaN(stats.corr([NaN, 1, 2], [1, 2, 3]), "NaN poisons corr (no throw)");

    /* MIXED input paths: Float64Array paired with number[] */
    const df = new Float64Array(DATA);
    const xf = new Float64Array(X2);
    assertSameBits(stats.cov(df, xf), stats.cov(DATA, X2), "cov f64/f64 == arr/arr");
    assertSameBits(stats.cov(df, X2), stats.cov(DATA, X2), "cov f64/arr == arr/arr");
    assertSameBits(stats.cov(DATA, xf), stats.cov(DATA, X2), "cov arr/f64 == arr/arr");
    assertSameBits(stats.corr(df, X2), stats.corr(DATA, X2), "corr mixed == arr/arr");

    /* differential vs the naive reference */
    const rand = lcg(23);
    for (let t = 0; t < 15; t++) {
        const len = 2 + Math.floor(rand() * 150);
        const a = [], b = [];
        for (let i = 0; i < len; i++) {
            a.push((rand() - 0.5) * 100);
            b.push(a[i] * 2 + (rand() - 0.5) * 10);
        }
        const af = new Float64Array(a), bf = new Float64Array(b);
        assert(Math.abs(stats.cov(a, b) - naiveCov(a, b, false)) <=
               1e-10 * Math.max(1, Math.abs(naiveCov(a, b, false))),
               "cov ~ naive n=" + len);
        assert(Math.abs(stats.corr(a, b) - naiveCorr(a, b)) <= 1e-10,
               "corr ~ naive n=" + len);
        assertSameBits(stats.cov(af, bf), stats.cov(a, b), "cov paths agree n=" + len);
        assertSameBits(stats.corr(af, bf), stats.corr(a, b), "corr paths agree n=" + len);
    }
}

/* ================================================================ *
 *  Input routing: array-likes, non-float typed arrays, refuse-juice
 * ================================================================ */
{
    assertEq(stats.sum({ length: 3, 0: 1, 1: 2, 2: 3 }), 6, "array-likes are accepted");
    assertThrows(function () { stats.sum(5); }, "TypeError", "number is not an array");
    assertThrows(function () { stats.mean(null); }, "TypeError", "null is not an array");
    /* a BigInt64Array shares the 8-byte stride but never its meaning: the
     * fallback reads elements as Numbers and BigInt conversion throws. */
    if (typeof BigInt64Array !== "undefined") {
        assertThrows(function () { stats.sum(new BigInt64Array([1, 2])); }, "TypeError",
                     "BigInt64Array is never reinterpreted as doubles");
    }
    /* Int32Array / Float32Array take the per-element fallback and work. */
    assertEq(stats.sum(new Int32Array([1, 2, 3])), 6, "Int32Array via fallback");
    const f32 = new Float32Array(2);
    f32[0] = 0.1; f32[1] = 0.2;
    /* elements are read as doubles OF THE FLOAT32 VALUES, i.e. the stored
     * bits, not re-rounded decimals */
    assertEq(stats.sum(f32), Math.fround(0.1) + Math.fround(0.2),
             "Float32Array elements read as doubles of the stored values");
    /* detached buffer on the fast path is an error, not garbage */
    if (typeof ArrayBuffer.prototype.transfer === "function") {
        const ab = new ArrayBuffer(16);
        const ta = new Float64Array(ab);
        ab.transfer();
        assertThrows(function () { stats.sum(ta); }, "TypeError", "detached buffer");
        /* loop-2 regression: a getter on the fallback side must not be able
         * to make the other side's raw pointer dangle. cov snapshots both
         * sides before folding, so the answer is computed from the values
         * that were read, never from freed memory. */
        const ab2 = new ArrayBuffer(32);
        const g = new Float64Array(ab2);
        g[0] = 1; g[1] = 2; g[2] = 3; g[3] = 4;
        let hit = false;
        const evil = {
            length: 4,
            0: 10, 1: 20, 2: 30,
            get 3() { if (!hit) { hit = true; ab2.transfer(); } return 40; },
        };
        assertEq(stats.cov(g, evil), 50 / 3,
                 "detach-during-fold cov computes from the snapshot");
        /* the reverse pairing snapshots the typed side the same way:
         * covariance is symmetric, and the answer is again the snapshot's */
        const ab3 = new ArrayBuffer(32);
        const g3 = new Float64Array(ab3);
        g3[0] = 1; g3[1] = 2; g3[2] = 3; g3[3] = 4;
        let hit3 = false;
        const evil3 = {
            length: 4,
            0: 10, 1: 20, 2: 30,
            get 3() { if (!hit3) { hit3 = true; ab3.transfer(); } return 40; },
        };
        assertEq(stats.cov(evil3, g3), 50 / 3,
                 "reverse pairing also computes from the snapshot");
        /* corr shares the snapshot rule; a fresh pair, since the getters
         * above already detached ab2 on their one shot */
        const ab4 = new ArrayBuffer(32);
        const g4 = new Float64Array(ab4);
        g4[0] = 1; g4[1] = 2; g4[2] = 3; g4[3] = 4;
        let hit4 = false;
        const evil4 = {
            length: 4,
            0: 10, 1: 20, 2: 30,
            get 3() { if (!hit4) { hit4 = true; ab4.transfer(); } return 40; },
        };
        assert(stats.corr(g4, evil4) > 0.9999999,
                 "corr detach path computes from the snapshot (r ~ 1)");
        /* review finding: the q ARGUMENT of quantile can run user JS too
         * (valueOf). q is coerced before the source is bound, so a detach
         * in the valueOf window is a clean TypeError, never a stale read
         * of the freed store (the old order returned garbage under ASan). */
        const ab5 = new ArrayBuffer(64 * 8);
        const f5 = new Float64Array(ab5);
        for (let i = 0; i < 64; i++) f5[i] = i;
        assertThrows(function () {
            stats.quantile(f5, { valueOf() { ab5.transfer(); return 0.5; } });
        }, "TypeError", "quantile q-valueOf detach is a clean TypeError");
        /* a q that detaches an INDEPENDENT buffer leaves the source alone */
        const ab6 = new ArrayBuffer(64 * 8);
        const f6 = new Float64Array(ab6);
        for (let i = 0; i < 64; i++) f6[i] = i;
        const qonce = { value: 0, valueOf() { this.value++; return 0.5; } };
        assertEq(stats.quantile(f6, qonce), 31.5,
                 "non-detaching q valueOf computes normally");
        assertEq(qonce.value, 1, "q valueOf ran exactly once");
    }
}

/* ================================================================ *
 *  bits.rotateRight8/16/32/64: the named twin of rotateLeft
 * ================================================================ */
{
    assertEq(bits.rotateRight8(0b11000001, 2), 0b01110000, "rotateRight8 known value");
    assertEq(bits.rotateRight16(0x8001, 1), 0xC000, "rotateRight16 known value");
    assertEq(bits.rotateRight32(0x80000000, 1), 0x40000000, "rotateRight32 known value");
    assertEq(bits.rotateRight64(0x8000000000000000n, 1), 0x4000000000000000n,
             "rotateRight64 known value");
    /* negative k behaves as rotateLeft(-k) — the exact mirror contract */
    assertEq(bits.rotateRight8(8, -1), 16, "rotateRight8(x, -1) == rotateLeft8(x, 1)");
    /* k is reduced modulo the width, so full-width k is the identity */
    assertEq(bits.rotateRight8(0xA5, 8), 0xA5, "rotateRight8 by the width is identity");
    assertEq(bits.rotateRight64(123456789n, 64), 123456789n, "rotateRight64 by 64 is identity");

    const x8 = 0b10010110, x16 = 0b1011000011110001, x32 = 0xDEADBEEF, x64 = 0x0123456789ABCDEFn;
    for (let k = -70; k <= 70; k++) {
        assertEq(bits.rotateRight8(x8, k), bits.rotateLeft8(x8, -k),
                 "rotateRight8 mirrors rotateLeft8 at k=" + k);
        assertEq(bits.rotateRight16(x16, k), bits.rotateLeft16(x16, -k),
                 "rotateRight16 mirrors rotateLeft16 at k=" + k);
        assertEq(bits.rotateRight32(x32, k), bits.rotateLeft32(x32, -k),
                 "rotateRight32 mirrors rotateLeft32 at k=" + k);
        assertEq(bits.rotateRight64(x64, k), bits.rotateLeft64(x64, -k),
                 "rotateRight64 mirrors rotateLeft64 at k=" + k);
    }
    /* round trip: rotating right by k then by (width - k) restores x */
    for (let k = 0; k < 8; k++) {
        assertEq(bits.rotateRight8(bits.rotateRight8(x8, k), 8 - k), x8,
                 "rotateRight8 round trip k=" + k);
    }
    for (let k = 0; k < 64; k += 7) {
        assertEq(bits.rotateRight64(bits.rotateRight64(x64, k), 64 - k), x64,
                 "rotateRight64 round trip k=" + k);
    }
}

/* ================================================================ *
 *  Whole-surface consistency: one big random matrix, both paths,
 *  every function, bit-for-bit.
 * ================================================================ */
{
    const rand = lcg(99);
    const a = [], b = [];
    for (let i = 0; i < 1000; i++) {
        a.push((rand() - 0.5) * 2);
        b.push((rand() - 0.5) * 2);
    }
    const af = new Float64Array(a), bf = new Float64Array(b);
    const pairs = [
        [function (x) { return stats.sum(x); }, "sum"],
        [function (x) { return stats.mean(x); }, "mean"],
        [function (x) { return stats.variance(x); }, "variance"],
        [function (x) { return stats.variance(x, { pop: true }); }, "variance pop"],
        [function (x) { return stats.stddev(x); }, "stddev"],
        [function (x) { return stats.median(x); }, "median"],
        [function (x) { return stats.quantile(x, 0.37); }, "quantile"],
        [function (x) { return stats.min(x); }, "min"],
        [function (x) { return stats.max(x); }, "max"],
    ];
    for (const [f, nm] of pairs) {
        assertSameBits(f(af), f(a), "matrix " + nm + " paths agree");
    }
    assertSameBits(stats.cov(af, bf), stats.cov(a, b), "matrix cov paths agree");
    assertSameBits(stats.corr(af, bf), stats.corr(a, b), "matrix corr paths agree");
}

/* ================================================================ *
 *  Cross-language oracle matrix (loop-2 evidence): 5000-element LCG
 *  dataset, every value below computed INDEPENDENTLY IN PYTHON
 *  (math.fsum/Neumaier + exact R-7) and pinned here bit-for-bit.
 *  See CHANGELOG.md — the comparison matrix, C vs Python: 13/13 exact.
 * ================================================================ */
{
    function lcgFixed(seed) {
        let s = seed >>> 0;
        return function () {
            s = (Math.imul(s, 1664525) + 1013904223) >>> 0;
            return s / 4294967296;
        };
    }
    const rand = lcgFixed(123456789);
    const a = [], b = [];
    for (let i = 0; i < 5000; i++) a.push((rand() - 0.5) * 2);
    for (let i = 0; i < 5000; i++) b.push((rand() - 0.5) * 100);
    const oracle = {
        sum: -38.43027369491756,
        mean: -0.007686054738983512,
        var_s: 0.33015998575274713,
        var_p: 0.3300939537555966,
        std_s: 0.5745954975047639,
        median: -0.015645087230950594,
        q25: -0.4948121889028698,
        q99: 0.978510055099614,
        min: -0.9998896741308272,
        max: 0.9998884191736579,
        cov_s: 0.23023595788052773,
        cov_p: 0.23018991068895162,
        corr: 0.013725977101271618,
    };
    assertEq(stats.sum(a), oracle.sum, "oracle sum");
    assertEq(stats.mean(a), oracle.mean, "oracle mean");
    assertEq(stats.variance(a), oracle.var_s, "oracle sample variance");
    assertEq(stats.variance(a, { pop: true }), oracle.var_p, "oracle population variance");
    assertEq(stats.stddev(a), oracle.std_s, "oracle sample stddev");
    assertEq(stats.median(a), oracle.median, "oracle median");
    assertEq(stats.quantile(a, 0.25), oracle.q25, "oracle q25");
    assertEq(stats.quantile(a, 0.99), oracle.q99, "oracle q99");
    assertEq(stats.min(a), oracle.min, "oracle min");
    assertEq(stats.max(a), oracle.max, "oracle max");
    assertEq(stats.cov(a, b), oracle.cov_s, "oracle sample covariance");
    assertEq(stats.cov(a, b, { pop: true }), oracle.cov_p, "oracle population covariance");
    assertEq(stats.corr(a, b), oracle.corr, "oracle correlation");
}

print("test_mathx_stats: all tests passed (" + n + " assertions)");
