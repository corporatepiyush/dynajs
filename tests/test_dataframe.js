/* dyna:dataframe -- the whole surface.
 *
 * Three things this file exists to catch, none of which a hand-written
 * "does sum work" test reaches:
 *
 * 1. TAIL BUGS. Every reduction kernel steps NACC elements at a time and cleans
 *    up with a scalar tail. A wrong tail is invisible at every length the step
 *    divides -- which is exactly the length a hand-written test picks. So every
 *    reduction runs over N_TAILS below, which brackets the tail of an 8-, 16-
 *    and 32-wide body from both sides. Nothing here names the accumulator
 *    count: it is a property of the C (DF_ACCS_FLOAT / DF_ACCS_INT) and has
 *    already been raised once, so pinning a number here would only rot.
 *
 * 2. THE VALUE CONTRACT. min/max IGNORE NaN and sum/mean/product PROPAGATE it;
 *    those two families disagree DELIBERATELY and the disagreement is the
 *    shipped behaviour. A masked-out element folds in the aggregate's IDENTITY,
 *    so a masked reduction EXCLUDES rather than zeroes -- which is why a
 *    masked-out +Infinity does not poison the answer. Each of those is a
 *    plausible-wrong-answer bug if it flips, never a crash, so each is pinned.
 *
 * 3. LIFETIME. The module aliases the caller's ArrayBuffer backing stores
 *    zero-copy, so the dangerous bug is not a wrong number: it is a column
 *    pointer that outlives the buffer. Every argument is coerced by code that
 *    can run arbitrary JS, and that JS can detach the buffer the method is
 *    about to read. The rule is coerce-everything-first, then bind. The attacks
 *    at the bottom go after exactly that -- and each one PROVES ITS HOOK FIRED,
 *    because an attack that hooks a coercion the method never performs passes
 *    while exercising nothing at all.
 *
 * Run:  dynajs tests/test_dataframe.js
 * Needs a binary built with CONFIG_NATIVE_MODULES=y; a plain `make` produces one
 * where the import below throws.
 */
import { DataFrame } from "dyna:dataframe";

/* ------------------------------------------------------------------ harness */

let pass = 0, fail = 0, section = "(none)";
const failures = [];

function S(name) { section = name; }
function bad(what, detail) {
    fail++;
    const line = "FAIL [" + section + "] " + what + (detail ? ": " + detail : "");
    failures.push(line);
    console.log(line);
}
function ok(cond, what, detail) { if (cond) pass++; else bad(what, detail); }

/* NaN-aware equality. Deliberately NOT Object.is: it would separate -0 from +0,
   and min/max leave the sign of zero unspecified (parallel accumulators, so
   which one a zero lands in depends on its index). `same` is the Object.is
   spelling, used only where the sign of zero IS specified. */
function eq(a, b, what) {
    ok((a === b) || (Number.isNaN(a) && Number.isNaN(b)), what, a + " !== " + b);
}
function same(a, b, what) { ok(Object.is(a, b), what, a + " is not " + b); }
/* For two results that are the same quantity by a DIFFERENT summation order.
   `eq` would pin one association and break the day an accumulator count moves;
   `near`'s relative tolerance is far looser than the few ulp actually at stake. */
function ulpNear(a, b, ulps, what) {
    const d = Math.abs(a - b), scale = Math.max(Math.abs(a), Math.abs(b));
    const got = scale === 0 ? 0 : d / (scale * Number.EPSILON);
    ok(got <= ulps, what, a + " vs " + b + " (" + got.toFixed(2) + " ulp > " + ulps + ")");
}
function near(a, b, what, tol) {
    tol = tol || 1e-12;
    if (Number.isNaN(a) && Number.isNaN(b)) { pass++; return; }
    ok(Math.abs(a - b) <= tol * Math.max(1, Math.abs(b)), what,
       a + " vs " + b + " (rel " + (Math.abs(a - b) / Math.max(1e-300, Math.abs(b))) + ")");
}
function throws(fn, what) {
    let threw = null;
    try { fn(); } catch (e) { threw = e; }
    ok(threw !== null, what + " did not throw");
    return threw;
}
/* A throw is only the right throw if it names the thing that is wrong; a
   TypeError from a typo in the test looks identical otherwise. */
function throwsLike(fn, needle, what) {
    const e = throws(fn, what);
    if (e) ok(String(e.message).indexOf(needle) >= 0, what + " message names '" + needle + "'",
              e.message);
}
/* Elementwise, reported as ONE assertion carrying the worst disagreement --
   otherwise a broken kernel prints 10000 lines and the count means nothing. */
function elemEq(got, want, what, tol) {
    let n = -1, worst = 0;
    if (got.length !== want.length) { bad(what, "length " + got.length + " != " + want.length); return; }
    for (let i = 0; i < want.length; i++) {
        const g = got[i], w = want[i];
        if (Object.is(g, w)) continue;
        if (Number.isNaN(g) && Number.isNaN(w)) continue;
        if (g === w) continue;                       /* +0 vs -0 where unspecified */
        const rel = Math.abs(g - w) / Math.max(1e-300, Math.abs(w));
        if (tol && rel <= tol) continue;
        if (n < 0 || rel > worst) { n = i; worst = rel; }
    }
    ok(n < 0, what, n < 0 ? "" : "first/worst at [" + n + "] got " + got[n] +
                                 " want " + want[n] + " rel " + worst);
}
const arr = (a) => Array.from(a);

/* ------------------------------------------------ 47-method coverage ledger */

/* Structural, not a hardcoded list of 47: the count has already drifted once
   (a written-down enumeration was missing `rsub`), and a list that has to be
   edited when a method is added is a list that will be wrong. Enumerating the
   prototype means a new method fails this file until it is covered. */
const touched = new Set();
function mark(...names) { for (const n of names) touched.add(n); }

/* `touched` only proves a name was TYPED: gutting a method's assertions while
   leaving its mark() still reported 158/158. The wrapper below records what was
   actually CALLED, and both ledgers are asserted at the bottom. */
const invoked = new Set();
{
    const proto = Object.getPrototypeOf(new DataFrame({ _p: new Float64Array(1) }));
    for (const k of Object.getOwnPropertyNames(proto)) {
        if (k === "constructor") continue;
        const d = Object.getOwnPropertyDescriptor(proto, k);
        if (!d || d.get || typeof d.value !== "function" || !d.writable) continue;
        const orig = d.value, arity = orig.length;
        const wrap = function () {
            invoked.add(k);
            return orig.apply(this, arguments);
        };
        /* keep .length: the arity sweep below reads it, and a wrapper's is 0 */
        Object.defineProperty(wrap, "length", { value: arity, configurable: true });
        Object.defineProperty(wrap, "name", { value: k, configurable: true });
        Object.defineProperty(proto, k, { ...d, value: wrap });
    }
}

/* ------------------------------------------------------- dtype/value tables */

/* Every numeric column type the module accepts, discovered from DF_FLOAT_TYPES
   and DF_INT_TYPES in src/dyna-dataframe.c. Uint8ClampedArray and the BigInt
   arrays are deliberately absent there and are attacked below instead. */
const NUMERIC = [
    ["f64", Float64Array, false], ["f32", Float32Array, false],
    ["i32", Int32Array, true], ["u32", Uint32Array, true],
    ["i16", Int16Array, true], ["u16", Uint16Array, true],
    ["i8", Int8Array, true], ["u8", Uint8Array, true],
];
const INTS = NUMERIC.filter((t) => t[2]);
const SIGNED = new Set(["i32", "i16", "i8"]);

/* Generators chosen so the JS reference is EXACT, not merely close: every value
   is representable in the column's own type, so reading it back gives the same
   double the kernel widened. A generator that overflows the type would make the
   reference disagree with the kernel for a reason that is not a bug. */
const GEN = {
    f64: (i) => ((i * 7) % 23) - 11.5,
    f32: (i) => ((i * 7) % 23) - 11.5,        /* halves: exact in binary32 */
    i32: (i) => ((i * 2654435761) | 0),
    u32: (i) => ((i * 2654435761) >>> 0),
    i16: (i) => (((i * 37) & 0xffff) - 32768),
    u16: (i) => ((i * 37) & 0xffff),
    i8: (i) => (((i * 29) & 0xff) - 128),
    u8: (i) => ((i * 29) & 0xff),
};
/* product overflows any of the above within a dozen elements, and comparing two
   infinities proves nothing. Small factors keep every partial product exact. */
const GENP = {
    f64: (i) => [1, -1, 2, 0.5, -2][i % 5],
    f32: (i) => [1, -1, 2, 0.5, -2][i % 5],
    i32: (i) => [1, -1, 2, 1, -2][i % 5],
    u32: (i) => [1, 3, 2, 1, 2][i % 5],
    i16: (i) => [1, -1, 2, 1, -2][i % 5],
    u16: (i) => [1, 3, 2, 1, 2][i % 5],
    i8: (i) => [1, -1, 2, 1, -2][i % 5],
    u8: (i) => [1, 3, 2, 1, 2][i % 5],
};
function build(T, gen, n) {
    const a = new T(n);
    for (let i = 0; i < n; i++) a[i] = gen(i);
    return a;
}
/* A mask that is neither all-ones nor all-zeros at ANY of the tail lengths --
   an all-ones mask cannot tell "excluded" from "included" apart. */
const maskFor = (n) => { const m = new Uint8Array(n); for (let i = 0; i < n; i++) m[i] = (i % 3 !== 1) ? 1 : 0; return m; };

/* Lengths. 8 and 16 and 32 are the widths a reduction body has used or may use;
   every neighbour of each is here so a tail that runs one short or one long is
   caught from both sides, and 0/1 bracket the whole thing. */
const N_TAILS = [0, 1, 2, 3, 5, 7, 8, 9, 15, 16, 17, 23, 25, 31, 32, 33];

/* =========================================================== construction */
S("construction");
{
    const f = new Float64Array([1, 2, 3, 4]);
    const i = new Int32Array([10, 20, 30, 40]);
    const df = new DataFrame({ f, i });
    eq(df.ROWS, 4, "rows");
    eq(df.COLS, 2, "cols");
    eq(df.COLUMNS.join(","), "f,i", "columns");

    /* Int32Array must NOT be read as Float32Array: both are 4 bytes, and the
       width-based guess other modules use would silently return garbage. */
    eq(df.SUM("i"), 100, "int32 column summed as ints, not floats");
    eq(df.SUM("f"), 10, "float64 column");
    mark("SUM");

    const d2 = new DataFrame({
        f32: new Float32Array([1.5, 2.5]), i16: new Int16Array([-1, -2]),
        u8: new Uint8Array([200, 100]),
    });
    eq(d2.SUM("f32"), 4, "float32");
    eq(d2.SUM("i16"), -3, "int16 is SIGNED");
    eq(d2.SUM("u8"), 300, "uint8 is UNSIGNED");

    throwsLike(() => new DataFrame({ a: new Float64Array(2), b: new Float64Array(3) }),
               "same length", "ragged columns");
    throws(() => new DataFrame({ a: new BigInt64Array(2) }), "BigInt64Array column");
    throws(() => new DataFrame({ a: [1, 2, 3], b: new Float64Array(2) }), "ragged with string col");
    throws(() => new DataFrame("nope"), "non-object argument");
    throwsLike(() => new DataFrame({ a: new Float64Array(1) }).SUM("nope"),
               "no such column", "unknown column");

    /* A view with a byte offset must be honoured, not read from the buffer base */
    eq(new DataFrame({ a: new Float64Array([1, 2, 3, 4]).subarray(1, 3) }).SUM("a"), 5,
       "subarray column respects byteOffset");
}

/* ========================================== reductions x dtype x tail length */
S("reduction sweep");
{
    let cells = 0;
    for (const [tag, T] of NUMERIC) {
        const isInt = !!NUMERIC.find((t) => t[0] === tag)[2];
        for (const n of N_TAILS) {
            const a = build(T, GEN[tag], n);
            const p = build(T, GENP[tag], n);
            const m = maskFor(n);
            const df = new DataFrame({ a });
            const dp = new DataFrame({ p });
            const at = tag + "[" + n + "]";
            cells++;

            /* references: plain sequential loops. The definition, not another
               optimised path -- a reference sharing code with the thing under
               test proves nothing. */
            let rs = 0, rmin = Infinity, rmax = -Infinity;
            for (let i = 0; i < n; i++) {
                rs += a[i];
                if (a[i] < rmin) rmin = a[i];
                if (a[i] > rmax) rmax = a[i];
            }
            let ms = 0, mc = 0, mmin = Infinity, mmax = -Infinity;
            for (let i = 0; i < n; i++) if (m[i]) {
                ms += a[i]; mc++;
                if (a[i] < mmin) mmin = a[i];
                if (a[i] > mmax) mmax = a[i];
            }
            let rp = 1, mp = 1;
            for (let i = 0; i < n; i++) { rp *= p[i]; if (m[i]) mp *= p[i]; }

            /* An integer column accumulates in int64 and rounds ONCE on the way
               out, so it is exact; a float column reassociates across the
               accumulators and needs a tolerance. That difference is the
               contract, so the two are asserted differently on purpose. */
            if (isInt) {
                eq(df.SUM("a"), rs, "sum " + at);
                eq(df.SUM("a", m), ms, "sum masked " + at);
            } else {
                near(df.SUM("a"), rs, "sum " + at);
                near(df.SUM("a", m), ms, "sum masked " + at);
            }
            /* min/max round nothing, so they are exact for every type. n=0
               selects nothing and the answer is `undefined`, NOT the identity:
               the identity is an implementation detail the count gates off. */
            eq(df.MIN("a"), n ? rmin : undefined, "min " + at);
            eq(df.MAX("a"), n ? rmax : undefined, "max " + at);
            eq(df.MIN("a", m), mc ? mmin : undefined, "min masked " + at);
            eq(df.MAX("a", m), mc ? mmax : undefined, "max masked " + at);
            eq(df.COUNT("a"), n, "count " + at);
            eq(df.COUNT("a", m), mc, "count masked " + at);
            near(df.MEAN("a"), n ? rs / n : NaN, "mean " + at);
            near(df.MEAN("a", m), mc ? ms / mc : NaN, "mean masked " + at);
            near(dp.PRODUCT("p"), rp, "product " + at);
            near(dp.PRODUCT("p", m), mp, "product masked " + at);

            /* DOT_PRODUCT against itself: sum of squares, same tail structure */
            let rd = 0, md = 0;
            for (let i = 0; i < n; i++) { rd += a[i] * a[i]; if (m[i]) md += a[i] * a[i]; }
            near(df.DOT_PRODUCT("a", "a"), rd, "dot " + at);
            near(df.DOT_PRODUCT("a", "a", m), md, "dot masked " + at);

            /* variance: two-pass, n-1 divisor, NaN below two selected rows */
            const varOf = (sel) => {
                let s = 0, k = 0;
                for (let i = 0; i < n; i++) if (sel(i)) { s += a[i]; k++; }
                if (k < 2) return NaN;
                const mu = s / k;
                let q = 0;
                for (let i = 0; i < n; i++) if (sel(i)) q += (a[i] - mu) * (a[i] - mu);
                return q / (k - 1);
            };
            near(df.VARIANCE("a"), varOf(() => true), "variance " + at, 1e-9);
            near(df.VARIANCE("a", m), varOf((i) => m[i]), "variance masked " + at, 1e-9);
            const vv = varOf(() => true);
            near(df.STDDEV("a"), Number.isNaN(vv) ? NaN : Math.sqrt(vv), "stddev " + at, 1e-9);

            if (isInt) {
                let ra = -1, ro = 0, rx = 0, ma = -1, mo = 0, mx = 0;
                for (let i = 0; i < n; i++) {
                    ra &= a[i]; ro |= a[i]; rx ^= a[i];
                    if (m[i]) { ma &= a[i]; mo |= a[i]; mx ^= a[i]; }
                }
                /* A signed column returns a signed result (matching `a &= x` in
                   JS after ToInt32); an unsigned one returns the unsigned value,
                   which JS's own operators cannot express. */
                const fix = (v) => SIGNED.has(tag) ? (v | 0) : (v >>> 0);
                if (n) {
                    eq(df.BITWISE_AND("a"), fix(ra), "BITWISE_AND " + at);
                    eq(df.BITWISE_OR("a"), fix(ro), "BITWISE_OR " + at);
                    eq(df.BITWISE_XOR("a"), fix(rx), "BITWISE_XOR " + at);
                }
                if (mc) {
                    eq(df.BITWISE_AND("a", m), fix(ma), "BITWISE_AND masked " + at);
                    eq(df.BITWISE_OR("a", m), fix(mo), "BITWISE_OR masked " + at);
                    eq(df.BITWISE_XOR("a", m), fix(mx), "BITWISE_XOR masked " + at);
                }
            }
        }
    }
    mark("SUM", "MIN", "MAX", "MEAN", "COUNT", "PRODUCT", "DOT_PRODUCT",
         "VARIANCE", "STDDEV", "BITWISE_AND", "BITWISE_OR", "BITWISE_XOR");
    ok(cells === NUMERIC.length * N_TAILS.length, "sweep covered every dtype x length",
       cells + " cells");
}

/* ================================================ empty-column identities */
S("empty identities");
{
    /* The one input that exposes a wrong accumulator SEED. Reseeding min from
       x[0] instead of +Infinity is invisible everywhere else. */
    for (const [tag, T] of NUMERIC) {
        const df = new DataFrame({ a: new T(0) });
        eq(df.SUM("a"), 0, "empty " + tag + " sum -> 0");
        eq(df.COUNT("a"), 0, "empty " + tag + " count -> 0");
        eq(df.PRODUCT("a"), 1, "empty " + tag + " product -> 1");
        eq(df.MIN("a"), undefined, "empty " + tag + " min -> undefined");
        eq(df.MAX("a"), undefined, "empty " + tag + " max -> undefined");
        eq(df.MEAN("a"), NaN, "empty " + tag + " mean -> NaN");
        eq(df.VARIANCE("a"), NaN, "empty " + tag + " variance -> NaN");
        eq(df.STDDEV("a"), NaN, "empty " + tag + " stddev -> NaN");
        eq(df.DOT_PRODUCT("a", "a"), 0, "empty " + tag + " dot -> 0");
        eq(df.ABS("a").length, 0, "empty " + tag + " abs -> length 0");
        eq(df.IS_NA("a").length, 0, "empty " + tag + " isna -> length 0");
        eq(df.GT("a", 0).length, 0, "empty " + tag + " gt -> length 0");
    }
    /* AND's identity is all-ones AT THE COLUMN'S WIDTH: an empty Uint8Array
       reduces to 255, matching what any non-empty u8 column can produce. */
    const ALL_ONES = { u8: 255, u16: 65535, u32: 4294967295 };
    for (const [tag, T] of INTS) {
        const df = new DataFrame({ a: new T(0) });
        eq(df.BITWISE_AND("a"), SIGNED.has(tag) ? -1 : ALL_ONES[tag],
           "empty " + tag + " BITWISE_AND -> all bits set at its own width");
        eq(df.BITWISE_OR("a"), 0, "empty " + tag + " BITWISE_OR -> 0");
        eq(df.BITWISE_XOR("a"), 0, "empty " + tag + " BITWISE_XOR -> 0");
    }
    eq(new DataFrame({ a: new Uint8Array([255, 255]) }).BITWISE_AND("a"), 255,
       "non-empty u8 BITWISE_AND is 255, as the empty case now is");

    /* all/any are vacuous over zero rows, and they take a MASK, never a column
       name -- with no argument at all they refuse rather than assuming one. */
    const e = new DataFrame({ a: new Float64Array(0) });
    eq(e.ALL(new Uint8Array(0)), true, "empty all -> true (vacuous)");
    eq(e.ANY(new Uint8Array(0)), false, "empty any -> false (vacuous)");
    eq(e.BITMASK(new Uint8Array(0)).length, 0, "empty bitmask -> 0 words");
    throwsLike(() => e.ALL(), "required", "all() with no mask refuses");
    throwsLike(() => e.ANY(), "required", "any() with no mask refuses");
    throwsLike(() => e.BITMASK(), "required", "bitmask() with no mask refuses");
    mark("ALL", "ANY", "BITMASK", "ABS", "IS_NA", "GT");
}

/* ============================================ NaN / +-Inf / -0 value contract */
S("NaN and infinity contract");
{
    /* Two lengths on purpose: 5 runs entirely in the scalar tail, 16 fills the
       wide body. A contract that holds in one and not the other is a tail bug
       wearing a value bug's clothes. */
    for (const n of [5, 16, 17]) {
        const nan = new Float64Array(n).fill(NaN);
        const d = new DataFrame({ nan });
        /* min/max IGNORE NaN: `v < acc` is false for a NaN so no accumulator can
           hold one, and an all-NaN column therefore surfaces the SEED. This is
           the case that catches a min reseeded from x[0]. */
        eq(d.MIN("nan"), Infinity, "all-NaN[" + n + "] min -> +Infinity (the seed)");
        eq(d.MAX("nan"), -Infinity, "all-NaN[" + n + "] max -> -Infinity (the seed)");
        eq(d.COUNT("nan"), n, "all-NaN[" + n + "] count is still the row count");
        /* sum/mean/product PROPAGATE it, because `acc += v` does */
        eq(d.SUM("nan"), NaN, "all-NaN[" + n + "] sum -> NaN");
        eq(d.MEAN("nan"), NaN, "all-NaN[" + n + "] mean -> NaN");
        eq(d.PRODUCT("nan"), NaN, "all-NaN[" + n + "] product -> NaN");
        eq(d.VARIANCE("nan"), NaN, "all-NaN[" + n + "] variance -> NaN");

        /* one NaN among reals, placed in the LAST slot so it lands in the tail
           at n=17 and in the body at n=16 */
        const mix = new Float64Array(n).fill(2); mix[n - 1] = NaN;
        const dm = new DataFrame({ mix });
        eq(dm.MIN("mix"), 2, "NaN at [" + (n - 1) + "] ignored by min");
        eq(dm.MAX("mix"), 2, "NaN at [" + (n - 1) + "] ignored by max");
        eq(dm.SUM("mix"), NaN, "NaN at [" + (n - 1) + "] propagates through sum");
    }
    /* Infinities are ordinary values to min/max */
    const inf = new DataFrame({ v: new Float64Array([1, Infinity, -Infinity, 2]) });
    eq(inf.MIN("v"), -Infinity, "min sees -Infinity as a value");
    eq(inf.MAX("v"), Infinity, "max sees +Infinity as a value");
    eq(inf.SUM("v"), NaN, "+Inf + -Inf is NaN in sum");
    eq(inf.PRODUCT("v"), -Infinity, "product of the same column");

    /* min/max leave the SIGN OF ZERO unspecified -- which accumulator a zero
       lands in depends on its index -- so this asserts the VALUE and
       deliberately does not assert the sign. Object.is here would be a test
       that fails for a reason that is not a bug. */
    const z = new DataFrame({ v: new Float64Array([-0, 0]) });
    eq(z.MIN("v"), 0, "min over -0 and +0 is zero (sign unspecified)");
    eq(z.MAX("v"), 0, "max over -0 and +0 is zero (sign unspecified)");
    eq(z.SUM("v"), 0, "sum over -0 and +0");

    /* clip PROPAGATES NaN, which is a DIFFERENT contract from the min/max
       reduction and must not be conflated: both comparisons are false so the
       value falls through. Written with fmin/fmax a NaN would become a bound. */
    const cl = new DataFrame({ v: new Float64Array([NaN, -5, 0, 5, 10, Infinity, -Infinity]) });
    elemEq(cl.CLIP("v", 0, 7), [NaN, 0, 0, 5, 7, 7, 0], "clip propagates NaN and clamps Infinity");
    throwsLike(() => cl.CLIP("v", NaN, 1), "NaN", "clip refuses a NaN bound");
    throwsLike(() => cl.CLIP("v", 5, 1), "exceeds", "clip refuses an inverted range");
    elemEq(cl.CLIP("v", 3, 3), [NaN, 3, 3, 3, 3, 3, 3], "clip with lo == hi is legal");
    mark("CLIP");
}

/* ======================================= masked reductions EXCLUDE, not zero */
S("masked exclusion");
{
    /* The pin the brief asked for, at three lengths: 5 is tail-only, 16 fills a
       wide body, 21 is body+tail. Multiplying by a 0/1 weight instead of
       selecting the identity would turn the masked-out infinities into NaN --
       a reachable silent wrong answer, since a column holding Infinity and a
       `lt` mask over it is an ordinary query. */
    const pat = [1, Infinity, 5, -Infinity, 2], pm = [1, 0, 1, 0, 1];
    for (const n of [5, 16, 21]) {
        const v = new Float64Array(n), m = new Uint8Array(n);
        for (let i = 0; i < n; i++) { v[i] = pat[i % 5]; m[i] = pm[i % 5]; }
        let ws = 0, wc = 0;
        for (let i = 0; i < n; i++) if (m[i]) { ws += v[i]; wc++; }
        const df = new DataFrame({ v });
        eq(df.SUM("v", m), ws, "masked sum excludes the infinities [" + n + "]");
        eq(df.MIN("v", m), 1, "masked min excludes -Infinity [" + n + "]");
        eq(df.MAX("v", m), 5, "masked max excludes +Infinity [" + n + "]");
        eq(df.COUNT("v", m), wc, "masked count [" + n + "]");
        ok(Number.isFinite(df.SUM("v", m)), "masked sum is FINITE, not NaN [" + n + "]");
        ok(Number.isFinite(df.DOT_PRODUCT("v", "v", m)),
           "masked DOT_PRODUCT is FINITE, not NaN [" + n + "]");
        ok(Number.isFinite(df.VARIANCE("v", m)),
           "masked variance is FINITE, not NaN [" + n + "]");
    }
    /* exactly the brief's case, spelled out */
    const df5 = new DataFrame({ v: new Float64Array([1, Infinity, 5, -Infinity, 2]) });
    const m5 = new Uint8Array([1, 0, 1, 0, 1]);
    eq(df5.SUM("v", m5), 8, "sum=8");
    eq(df5.MIN("v", m5), 1, "min=1");
    eq(df5.MAX("v", m5), 5, "max=5");
    eq(df5.COUNT("v", m5), 3, "count=3");

    /* an all-zero mask selects nothing, and every aggregate says so its own way */
    const v = new Float64Array([1, 5, 10, 5, 20]);
    const d = new DataFrame({ v });
    const zero = new Uint8Array(5);
    eq(d.SUM("v", zero), 0, "all-zero mask sum -> 0");
    eq(d.COUNT("v", zero), 0, "all-zero mask count -> 0");
    eq(d.MEAN("v", zero), NaN, "all-zero mask mean -> NaN");
    eq(d.MIN("v", zero), undefined, "all-zero mask min -> undefined");
    eq(d.MAX("v", zero), undefined, "all-zero mask max -> undefined");
    eq(d.PRODUCT("v", zero), 1, "all-zero mask product -> 1");
    eq(d.VARIANCE("v", zero), NaN, "all-zero mask variance -> NaN");
    eq(d.DOT_PRODUCT("v", "v", zero), 0, "all-zero mask dot -> 0");

    /* any NONZERO byte is true, not just 1 */
    eq(d.COUNT("v", new Uint8Array([0, 2, 0, 255, 0])), 2, "mask is truthiness, not equality to 1");
    /* a mask LONGER than the column is accepted; only shorter is refused */
    eq(d.SUM("v", new Uint8Array(9).fill(1)), 41, "mask longer than the column is accepted");
    throwsLike(() => d.SUM("v", new Uint8Array(2)), "at least", "mask shorter than the column");
    throwsLike(() => d.SUM("v", new Float64Array(5)), "at least", "8-byte-element mask refused");
    /* bytes-per-element is what is checked, so an Int8Array IS a legal mask */
    eq(d.SUM("v", new Int8Array([1, 0, 1, 0, 1])), 31, "Int8Array is a legal mask (bpe 1)");
    eq(d.SUM("v", null), 41, "null mask means no mask");
    eq(d.SUM("v", undefined), 41, "undefined mask means no mask");
    throws(() => d.SUM("v", [1, 1, 1, 1, 1]), "a plain Array is not a mask");

    /* the column and the mask may be the SAME buffer: both kernel pointers are
       restrict, but restrict is only violated by MODIFYING an aliased object
       and neither is written */
    const u = new Uint8Array([1, 0, 3]);
    eq(new DataFrame({ u }).SUM("u", u), 4, "a Uint8Array column aliased as its own mask");
}

/* ================================== integer accumulator is exact past 2^53 */
S("int64 accumulator");
{
    /* 2^22 elements of 2^31 sum to exactly 2^53, then two 1s take it two past.
       A double accumulator rounds 2^53+1 back to 2^53 (ties-to-even) and does
       it again for the second, so it reports 2^53 -- the exact 2^53+2 is only
       reachable if the accumulator is an integer that rounds ONCE on the way
       out. The JS loop below is that wrong answer, asserted as wrong so the
       test has teeth: if it ever agrees, the case stopped discriminating. */
    const WANT = 9007199254740994;              /* 2^53 + 2 */
    {
        const K = 1 << 22, u = new Uint32Array(K + 2);
        u.fill(2147483648, 0, K); u[K] = 1; u[K + 1] = 1;
        let naive = 0; for (let i = 0; i < K + 2; i++) naive += u[i];
        eq(new DataFrame({ u }).SUM("u"), WANT, "u32 sum is EXACT at 2^53+2");
        ok(naive !== WANT, "the double-accumulator JS loop is WRONG here (it is the control)",
           "naive gave " + naive);
        eq(naive, 9007199254740992, "and it is wrong in the specific way predicted");
    }
    {
        const K = 1 << 23, v = new Int32Array(K + 2);
        v.fill(1073741824, 0, K); v[K] = 1; v[K + 1] = 1;
        let naive = 0; for (let i = 0; i < K + 2; i++) naive += v[i];
        eq(new DataFrame({ v }).SUM("v"), WANT, "i32 sum is EXACT at 2^53+2");
        ok(naive !== WANT, "the double-accumulator JS loop is WRONG for i32 too",
           "naive gave " + naive);
    }
}

/* ========================================= variance numerical stability */
S("variance stability");
{
    /* Large mean, small spread -- a price or a timestamp column. The
       one-pass algebraic shortcut E[x^2] - mu^2 cancels catastrophically here,
       which is the whole reason the kernel is two-pass. Asserting only that the
       kernel is right would not show the two passes buy anything, so the naive
       formula is asserted to FAIL on the same data. */
    const n = 1000, a = new Float64Array(n);
    for (let i = 0; i < n; i++) a[i] = 1e12 + i;
    const df = new DataFrame({ a });

    /* exact reference: every partial sum is an integer below 2^53, mu is exactly
       representable, and x - mu is exact by Sterbenz, so this loop rounds
       nowhere that matters */
    let s = 0; for (let i = 0; i < n; i++) s += a[i];
    const mu = s / n;
    let q = 0; for (let i = 0; i < n; i++) q += (a[i] - mu) * (a[i] - mu);
    const exact = q / (n - 1);
    eq(exact, 83416.66666666667, "the reference itself is the value we think it is");

    near(df.VARIANCE("a"), exact, "variance survives a mean 7 orders above the spread", 1e-9);
    near(df.STDDEV("a"), Math.sqrt(exact), "stddev likewise", 1e-9);

    let s2 = 0; for (let i = 0; i < n; i++) s2 += a[i] * a[i];
    const naive = (s2 / n - mu * mu) * n / (n - 1);
    const naiveErr = Math.abs(naive - exact) / exact;
    ok(naiveErr > 0.5, "the one-pass E[x^2]-mu^2 formula FAILS on this data " +
       "(if it stops failing, this case no longer tests anything)",
       "naive=" + naive + " rel err " + naiveErr);

    /* fewer than two selected rows is NaN, not a division by zero */
    eq(new DataFrame({ z: new Float64Array(1) }).VARIANCE("z"), NaN, "variance of one row -> NaN");
    eq(new DataFrame({ z: new Float64Array([1, 2]) }).VARIANCE("z"), 0.5, "n-1 divisor, not n");
    const one = new DataFrame({ z: new Float64Array([1, 2, 3]) });
    eq(one.VARIANCE("z", new Uint8Array([1, 0, 0])), NaN, "one selected row -> NaN");
}

/* ============================================================= comparisons */
S("comparisons");
{
    const v = new Float64Array([1, 5, 10, 5, 20]);
    const df = new DataFrame({ v });
    eq(arr(df.GT("v", 5)).join(""), "00101", "GT");
    eq(arr(df.GE("v", 5)).join(""), "01111", "GE");
    eq(arr(df.LT("v", 5)).join(""), "10000", "LT");
    eq(arr(df.LE("v", 5)).join(""), "11010", "LE");
    eq(arr(df.EQ("v", 5)).join(""), "01010", "EQ");
    eq(arr(df.NE("v", 5)).join(""), "10101", "NE");

    /* The threshold is compared in DOUBLE, never truncated to the column's
       type: ge(int32col, 2.5) must not become >= 2. That truncation was also
       undefined behaviour for a NaN or out-of-range threshold. */
    const i = new DataFrame({ n: new Int32Array([1, 2, 3]) });
    eq(arr(i.GE("n", 2.5)).join(""), "001", "ge with a fractional threshold does not truncate");
    eq(arr(i.EQ("n", 1.5)).join(""), "000", "eq with a fractional threshold matches nothing");
    eq(arr(i.GT("n", NaN)).join(""), "000", "gt against NaN is all-false, not UB");
    eq(arr(i.LT("n", NaN)).join(""), "000", "lt against NaN is all-false");
    eq(arr(i.NE("n", NaN)).join(""), "111", "ne against NaN is all-true");
    eq(arr(i.GT("n", 1e300)).join(""), "000", "gt against a huge threshold is not UB");
    eq(arr(i.LT("n", -1e300)).join(""), "000", "lt against a huge negative threshold");

    /* every dtype takes the same path */
    for (const [tag, T] of NUMERIC) {
        const n = 17, a = build(T, GEN[tag], n);
        const d = new DataFrame({ a }), thr = a[3];
        const want = [];
        for (let k = 0; k < n; k++) want.push(a[k] > thr ? 1 : 0);
        elemEq(d.GT("a", thr), want, "gt over " + tag);
    }
    /* NaN never compares true, in any direction */
    const nn = new DataFrame({ v: new Float64Array([NaN, 1]) });
    eq(arr(nn.GT("v", 0)).join(""), "01", "NaN element is not >");
    eq(arr(nn.LT("v", 0)).join(""), "00", "NaN element is not <");
    eq(arr(nn.EQ("v", NaN)).join(""), "00", "NaN element is not == NaN");
    eq(arr(nn.NE("v", 1)).join(""), "10", "NaN element IS !=");
    mark("GT", "GE", "LT", "LE", "EQ", "NE");
}

/* ================================================== element-wise maps */
S("maps");
{
    /* Every map computes in DOUBLE and returns a Float64Array, so it is
       BIT-IDENTICAL to `for (i...) out[i] = f(col[i])` -- unlike the reductions,
       which reassociate. That claim is what is asserted: equality, no tolerance.
       If a libm here ever differs from the engine's Math.*, this is where it
       shows up, and the message reports the worst element. */
    const MAPS = [
        ["ABS", Math.abs], ["ROUND", Math.round], ["FLOOR", Math.floor],
        ["CEIL", Math.ceil], ["SQRT", Math.sqrt], ["LOG", Math.log],
        ["EXP", Math.exp], ["SIGN", Math.sign],
    ];
    for (const [tag, T] of NUMERIC) {
        const n = 17, a = build(T, GEN[tag], n);
        const d = new DataFrame({ a });
        for (const [op, f] of MAPS) {
            const want = [];
            /* exp of a large int32 overflows to Infinity in both; that agrees */
            for (let k = 0; k < n; k++) want.push(f(a[k]));
            elemEq(d[op]("a"), want, op + " over " + tag);
        }
    }
    /* the values where a hand-rolled round or sign goes wrong */
    const edge = new Float64Array([-0.2, -1.5, -2.5, 2.5, 0.49999999999999994,
                                  -0, 0, NaN, Infinity, -Infinity, 0.5, -0.5]);
    const de = new DataFrame({ edge });
    const wantRound = [], wantSign = [];
    for (let k = 0; k < edge.length; k++) { wantRound.push(Math.round(edge[k])); wantSign.push(Math.sign(edge[k])); }
    elemEq(de.ROUND("edge"), wantRound, "round matches Math.round on the awkward domain");
    elemEq(de.SIGN("edge"), wantSign, "sign matches Math.sign on the awkward domain");
    /* Math.round is half toward +Infinity; C round() is half AWAY FROM ZERO.
       These two rows are the entire difference and they are why round is not
       written as round(). */
    const r = de.ROUND("edge");
    eq(r[1], -1, "round(-1.5) is -1 (JS), not -2 (C round)");
    eq(r[2], -2, "round(-2.5) is -2 (JS), not -3 (C round)");
    eq(r[4], 0, "round(0.49999999999999994) is 0, not 1");
    /* the sign of zero IS specified for these two, unlike min/max */
    same(r[0], -0, "round(-0.2) is -0");
    same(de.SIGN("edge")[5], -0, "sign(-0) is -0");
    same(de.SIGN("edge")[6], 0, "sign(+0) is +0");

    /* abs(INT32_MIN) is 2147483648, which does not fit an int32 -- a
       type-preserving abs would be wrong on exactly this one input */
    const iv = new DataFrame({ b: new Int32Array([-2147483648, -1, 0, 1]) });
    elemEq(iv.ABS("b"), [2147483648, 1, 0, 1], "abs(INT32_MIN) is exact in double");
    elemEq(iv.IS_NA("b"), [0, 0, 0, 0], "isna on an integer column is all-false");
    elemEq(iv.NOT_NA("b"), [1, 1, 1, 1], "notna on an integer column is all-true");
    mark("ABS", "ROUND", "FLOOR", "CEIL", "SQRT", "LOG", "EXP", "SIGN");
}

/* =========================================== isna / notna / between / fillna */
S("predicates and fill");
{
    const v = new Float64Array([NaN, -5, 0, 5, 10, Infinity, -Infinity, -0]);
    const df = new DataFrame({ v });
    elemEq(df.IS_NA("v"), [1, 0, 0, 0, 0, 0, 0, 0], "isna: only NaN, not the infinities");
    elemEq(df.NOT_NA("v"), [0, 1, 1, 1, 1, 1, 1, 1], "notna is the complement");
    elemEq(df.BETWEEN("v", 0, 10), [0, 0, 1, 1, 1, 0, 0, 1], "between is inclusive at both ends");
    elemEq(df.BETWEEN("v", 10, 0), [0, 0, 0, 0, 0, 0, 0, 0],
           "an inverted range selects nothing -- between allows it, clip does not");
    elemEq(df.BETWEEN("v", NaN, 1), [0, 0, 0, 0, 0, 0, 0, 0], "a NaN bound selects nothing");
    elemEq(df.BETWEEN("v", -Infinity, Infinity), [0, 1, 1, 1, 1, 1, 1, 1],
           "an infinite range selects everything except NaN");
    elemEq(df.FILL_NA("v", -1), [-1, -5, 0, 5, 10, Infinity, -Infinity, -0],
           "fillna replaces only NaN");
    elemEq(df.FILL_NA("v", NaN), [NaN, -5, 0, 5, 10, Infinity, -Infinity, -0],
           "a NaN fill value is legal and is a no-op");
    for (const [tag, T] of NUMERIC) {
        const a = build(T, GEN[tag], 17);
        const d = new DataFrame({ a });
        const wi = [], wb = [];
        for (let k = 0; k < 17; k++) { wi.push(a[k] !== a[k] ? 1 : 0); wb.push(a[k] >= -1 && a[k] <= 1 ? 1 : 0); }
        elemEq(d.IS_NA("a"), wi, "isna over " + tag);
        elemEq(d.BETWEEN("a", -1, 1), wb, "between over " + tag);
    }
    mark("IS_NA", "NOT_NA", "BETWEEN", "FILL_NA");
}

/* ============================================================ binary ops */
S("binary ops");
{
    const x = new Float64Array([1, 2, 3]), y = new Float64Array([4, 5, 6]);
    const i = new Int32Array([10, 20, 30]);
    const df = new DataFrame({ x, y, i });
    /* the right operand is a COLUMN iff it is a PRIMITIVE string; anything else
       is coerced to a number, which is how `new String("y")` becomes NaN */
    elemEq(df.ADD("x", "y"), [5, 7, 9], "add column");
    elemEq(df.ADD("x", 10), [11, 12, 13], "add scalar");
    elemEq(df.SUB("x", "y"), [-3, -3, -3], "sub column");
    elemEq(df.SUB("x", 1), [0, 1, 2], "sub scalar");
    elemEq(df.MUL("x", "y"), [4, 10, 18], "mul column");
    elemEq(df.MUL("x", 2), [2, 4, 6], "mul scalar");
    elemEq(df.DIV("x", "y"), [0.25, 0.4, 0.5], "div column");
    elemEq(df.DIV("x", 0), [Infinity, Infinity, Infinity], "div by zero is Infinity, not a trap");
    elemEq(df.POW("x", "y"), [1, 32, 729], "pow column");
    elemEq(df.POW("x", 2), [1, 4, 9], "pow scalar");
    elemEq(df.RSUB("x", 100), [99, 98, 97], "rsub is k - col");
    elemEq(df.RDIV("x", 100), [100, 50, 100 / 3], "rdiv is k / col");
    /* a non-f64 left operand is widened once rather than emitting 64 kernels */
    elemEq(df.ADD("i", "y"), [14, 25, 36], "add with a non-f64 LEFT operand");
    elemEq(df.MUL("i", "i"), [100, 400, 900], "a column may be its own right operand");
    elemEq(df.ADD("x", "x"), [2, 4, 6], "add('x','x') aliases one buffer on both sides");
    /* rsub/rdiv take a number only: rsub(a,b) on two columns is sub(b,a) */
    throwsLike(() => df.RSUB("x", "y"), "swapped", "rsub refuses a column right operand");
    throwsLike(() => df.RDIV("x", "y"), "swapped", "rdiv refuses a column right operand");
    elemEq(df.ADD("x", new String("y")), [NaN, NaN, NaN],
           "a String OBJECT is not a column name, it coerces to NaN");
    elemEq(df.ADD("x", undefined), [NaN, NaN, NaN], "undefined right operand -> NaN");
    /* every dtype on the right */
    for (const [tag, T] of NUMERIC) {
        const a = build(T, GEN[tag], 17), b = new Float64Array(17).fill(1.5);
        const d = new DataFrame({ a, b });
        const w = [];
        for (let k = 0; k < 17; k++) w.push(b[k] + a[k]);
        elemEq(d.ADD("b", "a"), w, "add f64 + " + tag);
    }
    mark("ADD", "SUB", "MUL", "DIV", "POW", "RSUB", "RDIV");
}

/* ================================================================== where */
S("WHERE");
{
    const x = new Float64Array([1, 2, 3, 4]);
    const i = new Int32Array([10, 20, 30, 40]);
    const df = new DataFrame({ x, i });
    const m = new Uint8Array([1, 0, 1, 0]);
    elemEq(df.WHERE(m, "x", "i"), [1, 20, 3, 40], "where column/column");
    elemEq(df.WHERE(m, "x", 99), [1, 99, 3, 99], "where column/scalar");
    elemEq(df.WHERE(m, 99, "i"), [99, 20, 99, 40], "where scalar/column");
    elemEq(df.WHERE(m, 7, 8), [7, 8, 7, 8], "where scalar/scalar");
    elemEq(df.WHERE(m, "i", "i"), [10, 20, 30, 40], "where with a non-f64 column on both sides");
    elemEq(df.WHERE(new Uint8Array([2, 0, 255, 0]), 1, 0), [1, 0, 1, 0],
           "any nonzero mask byte selects the left side");
    throwsLike(() => df.WHERE(undefined, 1, 2), "required", "where refuses a missing mask");
    throws(() => df.WHERE(new Uint8Array(2), 1, 2), "where refuses a short mask");
    mark("WHERE");
}

/* ============================================================ DOT_PRODUCT */
S("DOT_PRODUCT pairs");
{
    /* Fifteen specialised kernels (the eight diagonals plus f64 against each
       other type); every remaining pair goes through the generic path, which
       widens 128 elements at a time into an L1-resident block. n is chosen to
       cross that block boundary twice AND leave a partial block, because a
       generic path that only ever sees one full block is untested. */
    const n = 257;
    const mk = (T, f) => { const a = new T(n); for (let k = 0; k < n; k++) a[k] = f(k); return a; };
    const cols = {
        f64: mk(Float64Array, (k) => (k % 7) + 0.5),
        f32: mk(Float32Array, (k) => (k % 5) + 0.25),
        i32: mk(Int32Array, (k) => (k % 11) - 5),
        u32: mk(Uint32Array, (k) => k % 13),
        i16: mk(Int16Array, (k) => (k % 13) - 6),
        u16: mk(Uint16Array, (k) => k % 101),
        i8: mk(Int8Array, (k) => (k % 9) - 4),
        u8: mk(Uint8Array, (k) => k % 251),
    };
    const df = new DataFrame(cols);
    const names = Object.keys(cols);
    let pairs = 0;
    for (const an of names) for (const bn of names) {
        let want = 0;
        for (let k = 0; k < n; k++) want += cols[an][k] * cols[bn][k];
        /* all these products are exact in double, so this is equality */
        eq(df.DOT_PRODUCT(an, bn), want, "dot " + an + " x " + bn);
        pairs++;
    }
    eq(pairs, 64, "every one of the 8x8 type pairs was dotted");
    /* argument order must not matter: (narrow, f64) is swapped before dispatch */
    eq(df.DOT_PRODUCT("i16", "f64"), df.DOT_PRODUCT("f64", "i16"), "dot is order-independent");
    eq(df.DOT_PRODUCT("i8", "u16"), df.DOT_PRODUCT("u16", "i8"), "generic path is order-independent");
    throwsLike(() => new DataFrame({ s: ["a"], v: new Float64Array(1) }).DOT_PRODUCT("s", "v"),
               "string", "dot refuses a string column");
    mark("DOT_PRODUCT");
}

/* ======================================================== all / any / bitmask */
S("all any bitmask");
{
    /* Three strategies must agree on every shape. The block form is what ships;
       the scan and early forms exist so the crossover can be re-measured, and if
       they ever disagree the crossover measurement is measuring two different
       functions. 4096 is the block size, so its neighbours are the interesting
       lengths. */
    for (const n of [0, 1, 7, 4095, 4096, 4097, 10000]) {
        const df = new DataFrame({ a: new Float64Array(n) });
        const shapes = {
            allTrue: new Uint8Array(n).fill(1),
            allFalse: new Uint8Array(n),
            firstOnly: (() => { const u = new Uint8Array(n); if (n) u[0] = 1; return u; })(),
            lastOnly: (() => { const u = new Uint8Array(n); if (n) u[n - 1] = 1; return u; })(),
            lastZero: (() => { const u = new Uint8Array(n).fill(1); if (n) u[n - 1] = 0; return u; })(),
            /* a byte of 2 is TRUE; ANDing the raw bytes would make 1 & 2 == 0 */
            twos: new Uint8Array(n).fill(2),
        };
        for (const [sn, sv] of Object.entries(shapes)) {
            let wantAny = false, wantAll = true;
            for (let i = 0; i < n; i++) { if (sv[i]) wantAny = true; else wantAll = false; }
            const at = " n=" + n + " " + sn;
            /* The four strategy variants were measurement-only and are gone:
               the blocked scan matched the best of them in every shape, so the
               crossover lives in the block size and not in the method name. */
            eq(df.ALL(sv), wantAll, "ALL" + at);
            eq(df.ANY(sv), wantAny, "ANY" + at);
        }
    }
    /* bitmask packs LSB-first, ceil(rows/32) words, bits past the last row zero.
       The lengths bracket the 32-wide SWAR body from both sides -- a tail that
       runs one word short is invisible at every multiple of 32. */
    for (const n of [0, 1, 2, 31, 32, 33, 63, 64, 65, 100, 127, 128, 129]) {
        const a = new Float64Array(n), m = new Uint8Array(n);
        for (let i = 0; i < n; i++) { a[i] = i; m[i] = (i % 3 === 0) ? (i % 7) + 1 : 0; }
        const bm = new DataFrame({ a }).BITMASK(m);
        eq(bm.length, Math.ceil(n / 32), "bitmask word count n=" + n);
        let bad = -1;
        for (let i = 0; i < n; i++)
            if (((bm[i >> 5] >>> (i & 31)) & 1) !== (m[i] ? 1 : 0)) { bad = i; break; }
        ok(bad < 0, "bitmask bits n=" + n, "first wrong bit at " + bad);
        /* bits past nrows in the final word must be zero, or a consumer reading
           whole words counts rows that do not exist */
        if (n && (n & 31)) {
            const stray = bm[bm.length - 1] >>> (n & 31);
            eq(stray, 0, "bitmask leaves no stray bits past row " + n);
        }
    }
    mark("ALL", "ANY", "BITMASK");
}

/* ================================================================ group by */
S("group by");
{
    const city = ["NY", "SF", "NY", "LA", "SF", "NY"];
    const amt = new Float64Array([1, 2, 3, 4, 5, 6]);
    const df = new DataFrame({ city, amt });
    const g = df.GROUP_BY_SUM("city", "amt");
    eq(g.keys.join(","), "NY,SF,LA", "group keys in first-appearance (dictionary) order");
    eq(arr(g.values).join(","), "10,7,4", "group sums");
    eq(arr(df.GROUP_BY_SUM("city", "amt", df.GT("amt", 2)).values).join(","), "9,5,4", "masked group sums");

    const k = new Int32Array([0, 1, 0, 2, 1, 0]);
    const d2 = new DataFrame({ k, amt });
    eq(d2.GROUP_BY_SUM("k", "amt").keys.join(","), "0,1,2", "integer group keys");
    eq(arr(d2.GROUP_BY_SUM("k", "amt").values).join(","), "10,7,4", "integer group sums");
    /* integer keys are DENSE 0..max, so a sparse column allocates the whole range */
    const sp = new DataFrame({ k: new Int32Array([0, 100]), v: new Float64Array([1, 2]) })
                    .GROUP_BY_SUM("k", "v");
    eq(sp.keys.length, 101, "integer keys are dense 0..max");
    eq(sp.values[100], 2, "and the far key still lands");
    /* every integer width may be a key */
    for (const [tag, T] of INTS) {
        const kk = new T([0, 1, 0]);
        const gg = new DataFrame({ kk, v: new Float64Array([1, 2, 3]) }).GROUP_BY_SUM("kk", "v");
        eq(arr(gg.values).join(","), "4,2", "group by a " + tag + " key column");
    }
    /* the value column may be any numeric type */
    eq(arr(new DataFrame({ k: new Int32Array([0, 1]), v: new Int32Array([7, 9]) })
              .GROUP_BY_SUM("k", "v").values).join(","), "7,9", "integer value column");
    /* NaN in the value column propagates into its own group only */
    const gn = new DataFrame({ k: new Int32Array([0, 1]), v: new Float64Array([NaN, 5]) })
                   .GROUP_BY_SUM("k", "v");
    eq(gn.values[0], NaN, "a NaN value poisons only its own group");
    eq(gn.values[1], 5, "the other group is untouched");

    throwsLike(() => d2.GROUP_BY_SUM("amt", "amt"), "integer or string", "float key column refused");
    throwsLike(() => new DataFrame({ k: new Int32Array([-1]), a: new Float64Array(1) })
                        .GROUP_BY_SUM("k", "a"), "negative", "negative group key refused");
    throwsLike(() => df.GROUP_BY_SUM("city", "city"), "string", "summing a string column refused");

    /* REGRESSION GUARD for the infinite loop fixed in da82d08. An EMPTY string
       column has an empty dictionary, so the group count is legitimately zero;
       clamping it to one fabricated a group whose key was read out of a NULL
       dictionary, and the process then spun on the faulting instruction forever.
       Note what a regression looks like here: this line HANGS rather than
       failing, so the breadcrumb below is the only thing that will name it. */
    console.log("  ... entering the GROUP_BY_SUM empty-dictionary regression guard");
    {
        const ge = new DataFrame({ k: [], v: new Float64Array(0) }).GROUP_BY_SUM("k", "v");
        eq(ge.keys.length, 0, "empty STRING key column -> zero groups (da82d08)");
        eq(ge.values.length, 0, "and zero values, the same number as keys");
    }
    /* The integer path agrees: no rows means no groups. It used to derive the
       count from the max key alone, and an empty column has max 0, so it
       fabricated one group holding zero. */
    {
        const gi = new DataFrame({ k: new Int32Array(0), v: new Float64Array(0) })
                       .GROUP_BY_SUM("k", "v");
        eq(gi.keys.length, 0, "empty INTEGER key column -> zero groups, as strings do");
        eq(gi.values.length, 0, "and zero values, the same number");
    }
    /* a string column of one empty string is a dictionary of size one, which is
       the neighbouring case and must still work */
    const g1 = new DataFrame({ k: [""], v: new Float64Array([7]) }).GROUP_BY_SUM("k", "v");
    eq(g1.keys.length, 1, "a dictionary holding only the empty string");
    eq(g1.values[0], 7, "and its sum");
    mark("GROUP_BY_SUM");
}

/* ======================================================================
   MEMORY SAFETY.
   Every attack below PROVES ITS HOOK FIRED. That is not ceremony: a string
   argument never reaches valueOf and a number argument never reaches toString,
   so an attack aimed at the wrong coercion passes having executed none of the
   user code it was written to inject. The measured mapping is:
       column NAME  -> toString  (JS_ToCString)
       any NUMBER   -> valueOf   (JS_ToFloat64)
       a MASK       -> neither; a TypedArray is read directly, so the only way
                       to attack a mask is through ANOTHER argument's coercion.
   ====================================================================== */
S("memory safety");

let hooksFired = 0;
/* run an attack and require that the injected code actually ran */
function attack(what, run) {
    let fired = 0;
    const fire = () => { fired++; hooksFired++; };
    let outcome;
    try { run(fire); outcome = "returned"; }
    catch (e) { outcome = "threw " + e.name; }
    ok(fired > 0, "REACHABLE: " + what + " -- the injected hook never ran, so this " +
       "attack exercised nothing");
    ok(true, "SURVIVED: " + what + " (" + outcome + ")");
}
const hostile = () => {
    const buf = new ArrayBuffer(8 * 64);
    const v = new Float64Array(buf);
    for (let i = 0; i < 64; i++) v[i] = i;
    return { df: new DataFrame({ v }), buf };
};

{   /* 1. detach the COLUMN buffer from inside the column-name coercion */
    attack("detach column buffer during the column-NAME coercion (toString)", (fire) => {
        const { df, buf } = hostile();
        return df.SUM({ toString() { fire(); buf.transfer(); return "v"; } });
    });
}
{   /* 2. detach it from inside the THRESHOLD coercion, which is valueOf and
       not toString -- an attack written against toString here runs nothing */
    attack("detach column buffer during the THRESHOLD coercion (valueOf)", (fire) => {
        const { df, buf } = hostile();
        return df.GT("v", { valueOf() { fire(); buf.transfer(); return 5; } });
    });
}
{   /* 3. detach during the SECOND column name, after the first is resolved */
    attack("detach during DOT_PRODUCT's SECOND column name", (fire) => {
        const { df, buf } = hostile();
        return df.DOT_PRODUCT("v", { toString() { fire(); buf.transfer(); return "v"; } });
    });
}
{   /* 4. detach the MASK buffer through an unrelated argument's coercion. This
       is the one that needs the argument order to be right: where() coerces its
       operands BEFORE it takes the mask pointer, so a detach here is caught. */
    attack("detach the MASK buffer during where()'s operand coercion", (fire) => {
        const mbuf = new ArrayBuffer(64);
        const mask = new Uint8Array(mbuf); mask.fill(1);
        const df = new DataFrame({ v: new Float64Array(64) });
        return df.WHERE(mask, { valueOf() { fire(); mbuf.transfer(); return 1; } }, 2);
    });
}
{   /* 5. detach the mask through the column-name coercion of a reduction */
    attack("detach the MASK buffer during a reduction's column-name coercion", (fire) => {
        const mbuf = new ArrayBuffer(64);
        const mask = new Uint8Array(mbuf); mask.fill(1);
        const df = new DataFrame({ v: new Float64Array(64) });
        return df.SUM({ toString() { fire(); mbuf.transfer(); return "v"; } }, mask);
    });
}
{   /* 6. RESIZE a resizable buffer smaller mid-coercion. The view stays
       ATTACHED, so a detach check alone misses this: the byte range has to be
       re-validated on every bind, not just the attached bit. */
    attack("shrink a resizable buffer mid-coercion (still attached!)", (fire) => {
        const rb = new ArrayBuffer(8 * 64, { maxByteLength: 8 * 64 });
        const df = new DataFrame({ v: new Float64Array(rb) });
        return df.SUM({ toString() { fire(); try { rb.resize(8); } catch (e) {} return "v"; } });
    });
}
{   /* 7. clip coerces lo and then hi; detach between the two */
    attack("detach between clip's lo and hi coercions", (fire) => {
        const { df, buf } = hostile();
        return df.CLIP("v", 0, { valueOf() { fire(); buf.transfer(); return 10; } });
    });
}
{   /* 8. a Proxy column map whose getter detaches while the CONSTRUCTOR is
       still walking columns */
    attack("Proxy column map that detaches during construction", (fire) => {
        const buf = new ArrayBuffer(64);
        const a = new Float64Array(8);
        let n = 0;
        const p = new Proxy({ a, b: new Float64Array(new ArrayBuffer(64)) }, {
            get(t, k) { if (n++ === 1) { fire(); buf.transfer(); } return t[k]; },
        });
        return new DataFrame(p).rows;
    });
}
{   /* 9. the binary right-hand operand is coerced with valueOf before binding */
    attack("detach during a binary op's scalar right operand", (fire) => {
        const { df, buf } = hostile();
        return df.ADD("v", { valueOf() { fire(); buf.transfer(); return 1; } });
    });
}
{   /* 10. GROUP_BY_SUM's value-column name, after the key column is named */
    attack("detach during GROUP_BY_SUM's SECOND column name", (fire) => {
        const buf = new ArrayBuffer(8 * 8);
        const v = new Float64Array(buf);
        const df = new DataFrame({ k: new Int32Array(8), v });
        return df.GROUP_BY_SUM("k", { toString() { fire(); buf.transfer(); return "v"; } });
    });
}

{   /* 11. the frame verbs' scalar arguments coerce BEFORE any bind. A hostile
       valueOf must fire before the source column is read. */
    attack("detach during SAMPLE's seed coercion", (fire) => {
        const { df, buf } = hostile();
        return df.SAMPLE(2, { valueOf() { fire(); buf.transfer(); return 42; } });
    });
}
{   /* 12. RESAMPLE's interval is a valueOf scalar; detach mid-coercion. */
    attack("detach during RESAMPLE's interval coercion", (fire) => {
        const { df, buf } = hostile();
        const t = new Float64Array(buf);
        const ts = new DataFrame({ t });
        return ts.RESAMPLE({ toString() { fire(); buf.transfer(); return "t"; } },
                           { valueOf() { fire(); return 5; } });
    });
}
{   /* 13. JOIN's `how` is a toString; the frames are coerced first. */
    attack("detach during JOIN's how coercion", (fire) => {
        const buf = new ArrayBuffer(8 * 64);
        const v = new Float64Array(buf);
        const df = new DataFrame({ k: new Int32Array(64), v });
        const other = new DataFrame({ k: Int32Array.from([1]), v: Float64Array.from([1]) });
        return df.JOIN(other, "k", "k", { toString() { fire(); buf.transfer(); return "inner"; } });
    });
}
{   /* 14. MASK's fill is valueOf; detach mid-coercion. */
    attack("detach during MASK's fill coercion", (fire) => {
        const { df, buf } = hostile();
        return df.MASK(new Uint8Array(64).fill(1),
                       { valueOf() { fire(); buf.transfer(); return 0; } });
    });
}
{   /* 15. PIVOT's agg is toString; detach mid-coercion. */
    attack("detach during PIVOT's agg coercion", (fire) => {
        const buf = new ArrayBuffer(8 * 8);
        const v = new Float64Array(buf);
        const df = new DataFrame({ k: new Int32Array(8), g: ["a", "b", "a", "b", "a", "b", "a", "b"], v });
        return df.PIVOT("k", "g", "v", { toString() { fire(); buf.transfer(); return "sum"; } });
    });
}
{   /* 16. JSON_AGG's mask argument coerce BEFORE the value column binds. */
    attack("detach during JSON_AGG's mask coercion", (fire) => {
        const { df, buf } = hostile();
        return df.JSON_AGG("v", { toString() { fire(); buf.transfer(); return "v"; } });
    });
}
{   /* 17. the `other` frame argument of a join is resolved before `this`'s
       columns bind; a hostile toString on the KEY must fire first. */
    attack("detach during JOIN's right key coercion", (fire) => {
        const { df, buf } = hostile();
        const other = new DataFrame({ k: Int32Array.from([1]), v: Float64Array.from([1]) });
        return df.JOIN(other, { toString() { fire(); buf.transfer(); return "k"; } }, "k");
    });
}

{   /* detached BEFORE the call: the plain case. Must throw, and the message
       must name the column, or the caller cannot tell which one is dead. */
    const buf = new ArrayBuffer(8 * 64);
    const v = new Float64Array(buf);
    const df = new DataFrame({ v });
    buf.transfer();
    throwsLike(() => df.SUM("v"), "detached", "sum on an already-detached buffer");
    throwsLike(() => df.MIN("v"), "detached", "min on an already-detached buffer");
    throwsLike(() => df.GT("v", 1), "detached", "gt on an already-detached buffer");
    throwsLike(() => df.ABS("v"), "detached", "abs on an already-detached buffer");
    throwsLike(() => df.CLIP("v", 0, 1), "detached", "clip on an already-detached buffer");
    throwsLike(() => df.WHERE(new Uint8Array(64), "v", 0), "detached",
               "where on an already-detached buffer");
    throwsLike(() => df.ADD("v", 1), "detached", "add on an already-detached buffer");
    throwsLike(() => df.PRODUCT("v"), "detached", "product on an already-detached buffer");
    throwsLike(() => df.VARIANCE("v"), "detached", "variance on an already-detached buffer");
    throwsLike(() => df.DOT_PRODUCT("v", "v"), "detached", "dot on an already-detached buffer");
    throwsLike(() => df.GROUP_BY_SUM("v", "v"), "detached", "GROUP_BY_SUM on a detached buffer");
    throws(() => df.SUM("v", new Uint8Array(64)), "masked sum on a detached column");
}
{   /* a detached MASK, present before the call */
    const mbuf = new ArrayBuffer(64);
    const mask = new Uint8Array(mbuf); mask.fill(1);
    const df = new DataFrame({ v: new Float64Array(64) });
    mbuf.transfer();
    throws(() => df.SUM("v", mask), "sum with an already-detached mask");
    throws(() => df.ALL(mask), "all with an already-detached mask");
    throws(() => df.BITMASK(mask), "bitmask with an already-detached mask");
}
{   /* a non-TypedArray where a column is expected. A plain Array is NOT an
       error: it is a string column, dictionary-encoded -- so the failure lands
       later, at the reduction, and it must be a throw and not a reduction over
       the dictionary codes. */
    const refuse = (what, v) =>
        throwsLike(() => new DataFrame({ a: v }), "expected", "column refused: " + what);
    refuse("null", null);
    refuse("undefined", undefined);
    refuse("a number", 5);
    refuse("a string", "abc");
    refuse("a DataView", new DataView(new ArrayBuffer(8)));
    refuse("Uint8ClampedArray", new Uint8ClampedArray(3));
    refuse("BigInt64Array", new BigInt64Array(2));
    refuse("BigUint64Array", new BigUint64Array(2));
    refuse("a bare ArrayBuffer", new ArrayBuffer(8));
    if (typeof SharedArrayBuffer === "function")
        refuse("a bare SharedArrayBuffer", new SharedArrayBuffer(8));
    refuse("a plain object", {});
    refuse("a function", function () {});

    const s = new DataFrame({ a: [1, 2, 3] });
    eq(s.ROWS, 3, "a plain Array becomes a dictionary-encoded string column");
    throwsLike(() => s.SUM("a"), "string column", "and reducing it is refused");
    throwsLike(() => s.ABS("a"), "string column", "and mapping it is refused");
    throwsLike(() => s.PRODUCT("a"), "string column", "and product is refused");
    throwsLike(() => s.VARIANCE("a"), "string column", "and variance is refused");
    throwsLike(() => s.BITWISE_AND("a"), "integer columns", "and BITWISE_AND names the dtypes");
    /* Comparing a string column numerically would answer about the dictionary
       codes, not the data, so the whole comparison surface refuses it by name
       as the reductions do. Every operator routes through one entry point. */
    for (const op of ["GT", "GE", "LT", "LE", "EQ", "NE"])
        throwsLike(() => s[op]("a", 0), "string column",
                   op + " on a string column is refused, not answered in codes");

    /* a TypedArray over a SharedArrayBuffer IS a legal column */
    if (typeof SharedArrayBuffer === "function") {
        const sab = new Float64Array(new SharedArrayBuffer(24));
        sab[0] = 1; sab[1] = 2; sab[2] = 3;
        eq(new DataFrame({ sab }).SUM("sab"), 6, "a Float64Array over a SharedArrayBuffer");
    }
    /* a bitwise reduction on a float column is refused BY NAME, never coerced:
       silently truncating a Float64Array to int32 and ANDing would return a
       plausible number */
    throwsLike(() => new DataFrame({ f: new Float64Array([1.5]) }).BITWISE_AND("f"),
               "Float64Array", "BITWISE_AND names the offending column type");
    throwsLike(() => new DataFrame({ f: new Float32Array([1.5]) }).BITWISE_OR("f"),
               "Float32Array", "BITWISE_OR names the offending column type");
}

/* --------------------------------------------- zero-copy is really zero-copy */
S("aliasing and lifetime");
{
    const v = new Float64Array([1, 2, 3]);
    const df = new DataFrame({ v });
    eq(df.SUM("v"), 6, "before mutation");
    v[0] = 100;
    eq(df.SUM("v"), 105, "the frame sees writes made through the original TypedArray");

    /* two columns viewing ONE buffer is legal and the kernels must not assume
       otherwise -- the map kernels restrict only their OUTPUT for this reason */
    const shared = new Float64Array([1, 2, 3, 4]);
    const d2 = new DataFrame({ a: shared.subarray(0, 2), b: shared.subarray(2, 4) });
    elemEq(d2.ADD("a", "b"), [4, 6], "two columns over one buffer");
}
{
    /* Frames dropped without any explicit close must be reclaimed. Growth here
       would mean the buffer refs are never released. */
    for (let r = 0; r < 2000; r++) {
        const v = new Float64Array(64);
        const c = [];
        for (let i = 0; i < 64; i++) c.push("k" + (i & 7));
        new DataFrame({ v, c }).GROUP_BY_SUM("c", "v");
    }
    ok(true, "2000 frames created and dropped without close()");
}

/* The generated sections below include rows that were written to be pasted
   INSIDE the "detached BEFORE the call" block above and so reference its `df`.
   Rather than re-brace each one, the same frame is bound once here, module
   scoped, already detached. Inner blocks that declare their own `df` shadow it
   normally. */
var df = (() => {
    const b = new ArrayBuffer(8 * 64);
    const v = new Float64Array(b);
    const d = new DataFrame({ v, k: new Int32Array(64) });
    b.transfer();
    return d;
})();

/* ================================================ generated: g1 */

/* ==================================================== ordering and quantiles */

/* The contract, written out the slow obvious way. None of these share code
   with the module and none of them share the module's ARITHMETIC either --
   medianByDefinition takes the middle of a sorted array rather than evaluating
   q*(m-1), and rankByDefinition counts comparisons rather than walking runs.
   A reference that transcribes the implementation's formula proves only that
   the formula was transcribed twice. */

/* ascending, NaN LAST, ties by row index -- returns [value, row] pairs */
function refOrder(vals, mask, desc) {
    const items = [];
    for (let i = 0; i < vals.length; i++) {
        if (mask && !mask[i]) continue;
        items.push([vals[i], i]);
    }
    items.sort((a, b) => {
        const na = Number.isNaN(a[0]), nb = Number.isNaN(b[0]);
        if (na || nb) { if (na !== nb) return na ? 1 : -1; }
        else if (a[0] !== b[0]) return (a[0] < b[0]) === !desc ? -1 : 1;
        return a[1] - b[1];
    });
    return items;
}
const refSorted = (vals, mask) => refOrder(vals, mask, false).map((p) => p[0]);
const refIndex = (vals, mask) => refOrder(vals, mask, false).map((p) => p[1]);
const refValued = (vals, mask) =>
    refSorted(vals, mask).filter((x) => !Number.isNaN(x));

/* the median as everyone defines it: the middle of the sorted values, or the
   mean of the two middles. No interpolation formula anywhere near it. */
function medianByDefinition(vals, mask) {
    const v = refValued(vals, mask);
    if (v.length === 0) return undefined;
    const h = v.length >> 1;
    return (v.length & 1) ? v[h] : (v[h - 1] + v[h]) / 2;
}
/* PERCENTILE_DISC by its DEFINITION -- the smallest value whose cumulative
   fraction reaches q -- searched for, not computed with ceil(q*m)-1 */
function discByDefinition(vals, mask, q) {
    const v = refValued(vals, mask);
    if (v.length === 0) return undefined;
    for (const cand of v) {
        let le = 0;
        for (const x of v) if (x <= cand) le++;
        if (le / v.length >= q) return cand;
    }
    return v[v.length - 1];
}
/* average rank by its definition: 1 + (strictly less) + (equal - 1)/2 */
function rankByDefinition(vals, mask) {
    const out = new Array(vals.length).fill(NaN);
    const sel = [];
    for (let i = 0; i < vals.length; i++)
        if ((!mask || mask[i]) && !Number.isNaN(vals[i])) sel.push(i);
    for (const i of sel) {
        let less = 0, equal = 0;
        for (const j of sel) {
            if (vals[j] < vals[i]) less++;
            else if (vals[j] === vals[i]) equal++;
        }
        out[i] = less + (equal + 1) / 2;
    }
    return out;
}

/* -0 and +0 are adjacent on purpose: they tie under < and >, so their order is
   settled by the row-index tie-break and by nothing else. Two 3s cover ties,
   the NaN covers the suffix rule. */
const ORD = new Float64Array([3, NaN, 1, 3, -0, 0, -5]);
const ORDMASK = new Uint8Array([1, 1, 1, 0, 0, 1, 1]);

S("sort / argsort");
{
    const df = new DataFrame({ v: ORD });
    elemEq(df.SORT("v"), [-5, -0, 0, 1, 3, 3, NaN], "sort: ascending, NaN LAST");
    elemEq(df.ARG_SORT("v"), [6, 4, 5, 2, 0, 3, 1], "argsort: the row permutation");
    ok(df.SORT("v") instanceof Float64Array, "sort returns a Float64Array");
    ok(df.ARG_SORT("v") instanceof Uint32Array, "argsort returns a Uint32Array");

    /* the sign of zero IS specified here, unlike min/max: the index tie-break
       is the only thing that can order two values that compare equal */
    const s = df.SORT("v");
    same(s[1], -0, "sort: -0 comes before +0 (row 4 before row 5)");
    same(s[2], 0, "sort: +0 comes second");

    /* NaN last is a CHOICE. A comparator that forgets NaN reports every NaN
       pair as equal and leaves the permutation unspecified; a NaN-first rule
       would put row 1 at index 0 in both of these. */
    eq(df.SORT("v")[6], NaN, "sort: the NaN is at the END, not the front");
    eq(df.ARG_SORT("v")[6], 1, "argsort: the NaN's row is last");

    /* a permutation keeps every selected row, NaN included -- unlike
       nlargest/nsmallest, which drop them */
    eq(df.SORT("v").length, 7, "sort is a PERMUTATION: nothing is dropped");
    eq(df.ARG_SORT("v").length, 7, "argsort is a permutation too");

    /* STABILITY. 64 equal keys have 64! orders that an unstable qsort may
       return; only the row-index tie-break makes it reproducible. */
    const flat = new DataFrame({ v: new Float64Array(64).fill(7) });
    elemEq(flat.ARG_SORT("v"), Array.from({ length: 64 }, (_, i) => i),
           "argsort of 64 equal keys is the IDENTITY, not an arbitrary order");
    const twoval = new DataFrame({ v: build(Int32Array, (i) => i & 1, 40) });
    const ti = arr(twoval.ARG_SORT("v"));
    ok(ti.slice(0, 20).every((x, i) => x === 2 * i) &&
       ti.slice(20).every((x, i) => x === 2 * i + 1),
       "argsort of a two-valued column keeps row order inside each value");

    /* the mask EXCLUDES: the output is the popcount long, and the indices are
       FRAME rows, so they still address every other column */
    elemEq(df.SORT("v", ORDMASK), [-5, 0, 1, 3, NaN], "sort: masked-out rows excluded");
    elemEq(df.ARG_SORT("v", ORDMASK), [6, 5, 2, 0, 1], "argsort: indices are FRAME rows");
    eq(df.SORT("v", ORDMASK).length, 5, "sort's length is the mask's popcount");
    const other = new Float64Array([10, 11, 12, 13, 14, 15, 16]);
    const df2 = new DataFrame({ v: ORD, w: other });
    elemEq(arr(df2.ARG_SORT("v", ORDMASK)).map((r) => other[r]), [16, 15, 12, 10, 11],
           "argsort's indices read another column of the same frame");

    /* empty and all-NaN */
    eq(new DataFrame({ v: new Float64Array(0) }).SORT("v").length, 0, "sort of an empty frame");
    eq(new DataFrame({ v: new Float64Array(0) }).ARG_SORT("v").length, 0, "argsort of an empty frame");
    elemEq(new DataFrame({ v: new Float64Array([NaN, NaN]) }).SORT("v"), [NaN, NaN],
           "an all-NaN column is permuted, not filtered");
    eq(new DataFrame({ v: ORD }).SORT("v", new Uint8Array(7)).length, 0,
       "an all-zero mask selects nothing");

    /* a dictionary code is insertion order, not lexicographic order: ordering
       codes would return a meaningless permutation that looks like an answer */
    const sdf = new DataFrame({ s: ["b", "a", "c"] });
    throwsLike(() => sdf.SORT("s"), "string column", "sort refuses a string column");
    throwsLike(() => sdf.ARG_SORT("s"), "string column", "argsort refuses a string column");
    throwsLike(() => sdf.SORT("nope"), "no such column", "sort on an unknown column");
    throws(() => df.SORT("v", new Uint8Array(1)), "sort with a short mask");
    mark("SORT", "ARG_SORT");
}

S("ordering sweep x dtype x tail length");
{
    let cells = 0, bad = 0;
    for (const [tag, T] of NUMERIC) {
        for (const n of N_TAILS) {
            const a = build(T, GEN[tag], n);
            const vals = arr(a);
            const m = maskFor(n);
            const df = new DataFrame({ a });
            for (const mask of [undefined, m]) {
                const want = refSorted(vals, mask), wi = refIndex(vals, mask);
                const got = arr(df.SORT("a", mask));
                const gi = arr(df.ARG_SORT("a", mask));
                if (got.length !== want.length || got.some((x, i) => x !== want[i])) bad++;
                else if (gi.some((r, i) => vals[r] !== want[i])) bad++;
                else if (gi.some((r, i) => r !== wi[i])) bad++;
                cells++;
            }
        }
    }
    ok(bad === 0, "sort/argsort match the reference over every dtype x length x mask",
       bad + " of " + cells + " cells disagree");
    ok(cells === NUMERIC.length * N_TAILS.length * 2, "the sweep ran every cell",
       "cells " + cells);
}

S("RANK");
{
    /* the tie rule, pinned against BOTH alternatives: 'MIN' gives [1,2,2,4]
       and 'dense' gives [1,2,2,3]; only 'average' gives 2.5 and 4 */
    const df = new DataFrame({ v: new Float64Array([10, 20, 20, 30]) });
    elemEq(df.RANK("v"), [1, 2.5, 2.5, 4], "rank: 1-based, ties AVERAGED");
    eq(df.RANK("v")[1], 2.5, "a tie is 2.5, not 2 ('MIN') ...");
    eq(df.RANK("v")[3], 4, "... and the row after it is 4, not 3 ('dense')");
    eq(df.RANK("v")[0], 1, "ranks are 1-BASED: the smallest is 1, not 0");
    ok(df.RANK("v") instanceof Float64Array, "rank returns a Float64Array");

    /* the reason average was chosen: the column total is invariant at
       n(n+1)/2 whatever the ties are, so a rank column composes with the sum
       and mean this module already ships. 'MIN' and 'dense' both break it. */
    const rdf = new DataFrame({ v: new Float64Array([5, 5, 5, 5, 5, 1, 9, 9]) });
    eq(new DataFrame({ r: rdf.RANK("v") }).SUM("r"), 36, "the ranks sum to n(n+1)/2 = 36");
    eq(new DataFrame({ r: df.RANK("v") }).SUM("r"), 10, "and to 10 for the four-row case");

    /* an unranked row is NaN, never 0: a 0 rank is a plausible wrong answer
       and would drag every downstream mean down */
    const nd = new DataFrame({ v: ORD });
    elemEq(nd.RANK("v"), [5.5, NaN, 4, 5.5, 2.5, 2.5, 1], "rank: a NaN row is unranked");
    elemEq(nd.RANK("v", ORDMASK), [4, NaN, 3, NaN, NaN, 2, 1],
           "rank: full height, masked-out rows are NaN");
    eq(nd.RANK("v", ORDMASK).length, 7,
       "rank is the ONE method here that returns a full-height column");

    elemEq(new DataFrame({ v: new Float64Array([1, 2, 3]) }).RANK("v"), [1, 2, 3],
           "a strictly increasing column ranks 1..n");
    eq(new DataFrame({ v: new Float64Array(0) }).RANK("v").length, 0, "rank of an empty frame");
    elemEq(new DataFrame({ v: new Float64Array([NaN, NaN]) }).RANK("v"), [NaN, NaN],
           "an all-NaN column ranks nothing");
    throwsLike(() => new DataFrame({ s: ["a"] }).RANK("s"), "string column",
               "rank refuses a string column");

    let cells = 0, bad = 0;
    for (const [tag, T] of NUMERIC) {
        for (const n of N_TAILS) {
            const a = build(T, GEN[tag], n);
            const vals = arr(a), m = maskFor(n);
            const d = new DataFrame({ a });
            for (const mask of [undefined, m]) {
                const want = rankByDefinition(vals, mask);
                const got = arr(d.RANK("a", mask));
                if (got.length !== n) bad++;
                else if (got.some((x, i) => !(x === want[i] ||
                                              (Number.isNaN(x) && Number.isNaN(want[i]))))) bad++;
                cells++;
            }
        }
    }
    ok(bad === 0, "rank matches the counting definition over every dtype x length x mask",
       bad + " of " + cells + " cells disagree");
    mark("RANK");
}

S("median / quantile / PERCENTILE_CONT / PERCENTILE_DISC");
{
    const df = new DataFrame({ v: ORD });
    /* median is PERCENTILE_CONT(0.5) under a different name -- one C function.
       These three must agree because there is only one implementation. */
    eq(df.MEDIAN("v"), 0.5, "median ignores the NaN and interpolates an even count");
    eq(df.QUANTILE("v", 0.5), df.MEDIAN("v"), "quantile(0.5) IS median");
    eq(df.PERCENTILE_CONT("v", 0.5), df.MEDIAN("v"), "PERCENTILE_CONT(0.5) IS median");
    eq(df.MEDIAN("v"), medianByDefinition(arr(ORD)), "median matches the definition");

    /* an even count INTERPOLATES; an odd count does not */
    const e = new DataFrame({ v: new Float64Array([1, 2, 3, 4]) });
    eq(e.MEDIAN("v"), 2.5, "median of an even count is the mean of the two middles");
    eq(new DataFrame({ v: new Float64Array([1, 2, 3]) }).MEDIAN("v"), 2, "odd count");

    /* the ORDER-STATISTIC contract: NaN is ignored (as min/max do) and an
       empty selection is `undefined` (as min/max do) -- NOT NaN, which is what
       mean and variance return. This deliberately differs from pandas. */
    eq(new DataFrame({ v: new Float64Array([1, NaN, 3]) }).MEDIAN("v"), 2,
       "median IGNORES NaN, exactly as min and max do");
    const ef = new DataFrame({ v: new Float64Array(0) });
    same(ef.MEDIAN("v"), undefined, "median of an empty frame is undefined");
    ok(!Number.isNaN(ef.MEDIAN("v")), "...and it is undefined, NOT NaN");
    same(ef.MIN("v"), undefined, "(min agrees -- this is the module's rule, not a new one)");
    ok(Number.isNaN(ef.MEAN("v")), "(mean disagrees -- arithmetic statistics give NaN)");
    same(new DataFrame({ v: new Float64Array([NaN, NaN]) }).MEDIAN("v"), undefined,
         "median of an all-NaN column is undefined");
    same(ef.QUANTILE("v", 0.9), undefined, "quantile of an empty frame is undefined");
    same(ef.PERCENTILE_DISC("v", 0.9), undefined, "PERCENTILE_DISC of an empty frame is undefined");
    same(df.MEDIAN("v", new Uint8Array(7)), undefined, "an all-zero mask leaves nothing to order");

    /* q = 0 and q = 1 are the extremes, which links this family to the ones
       already shipped */
    for (const [tag, T] of NUMERIC) {
        const a = build(T, GEN[tag], 25);
        const d = new DataFrame({ a });
        eq(d.QUANTILE("a", 0), d.MIN("a"), "quantile(0) is min over " + tag);
        eq(d.QUANTILE("a", 1), d.MAX("a"), "quantile(1) is max over " + tag);
    }

    /* CONT interpolates and may return a value the column does not contain;
       DISC must return one it does. An integer column is where they visibly
       part company. */
    const ints = new DataFrame({ v: new Int32Array([1, 2, 3, 4]) });
    eq(ints.PERCENTILE_CONT("v", 0.5), 2.5, "PERCENTILE_CONT(0.5) of [1,2,3,4] is 2.5");
    eq(ints.PERCENTILE_DISC("v", 0.5), 2, "PERCENTILE_DISC(0.5) is 2 -- a value PRESENT");
    ok(!Number.isInteger(ints.PERCENTILE_CONT("v", 0.5)),
       "the interpolated answer is NOT in an integer column");
    eq(ints.PERCENTILE_CONT("v", 0.25), 1.75, "PERCENTILE_CONT(0.25) is 1.75");
    eq(ints.PERCENTILE_DISC("v", 0.25), 1, "PERCENTILE_DISC(0.25) is 1");
    eq(ints.PERCENTILE_DISC("v", 0.26), 2, "PERCENTILE_DISC steps at the cumulative fraction");
    eq(ints.PERCENTILE_DISC("v", 0), 1, "PERCENTILE_DISC(0) is the minimum");
    eq(ints.PERCENTILE_DISC("v", 1), 4, "PERCENTILE_DISC(1) is the maximum");

    /* every PERCENTILE_DISC answer over an integer column must be an integer
       that the column contains, at every q -- and must match the definition */
    {
        const a = build(Int32Array, GEN.i32, 33);
        const d = new DataFrame({ a });
        const set = new Set(arr(a));
        let bad = 0, checked = 0;
        for (let q = 0; q <= 1.00001; q += 0.01) {
            const p = Math.min(q, 1);
            const got = d.PERCENTILE_DISC("a", p);
            if (!Number.isInteger(got) || !set.has(got)) bad++;
            else if (got !== discByDefinition(arr(a), undefined, p)) bad++;
            checked++;
        }
        ok(bad === 0, "PERCENTILE_DISC always returns a value the column contains",
           bad + " of " + checked + " values of q are wrong");
    }

    /* 0 * Infinity is NaN, so the interpolation form alone returns NaN for any
       quantile that lands exactly on a row of a column holding an infinity.
       The frac == 0 branch in the C is what stops that and is load-bearing. */
    const inf = new DataFrame({ v: new Float64Array([1, 2, Infinity]) });
    eq(inf.MEDIAN("v"), 2, "median of [1,2,Infinity] is 2, NOT NaN");
    eq(inf.QUANTILE("v", 0), 1, "quantile(0) of an infinite column is exact");
    eq(inf.QUANTILE("v", 1), Infinity, "quantile(1) is the infinity itself");
    eq(inf.QUANTILE("v", 0.75), Infinity, "an interpolation INTO the infinity is Infinity");
    eq(new DataFrame({ v: new Float64Array([-Infinity, 0, Infinity]) }).MEDIAN("v"), 0,
       "median of [-Inf, 0, Inf] is 0");

    /* masked */
    eq(df.MEDIAN("v", ORDMASK), 0.5, "median honours the mask");
    eq(df.PERCENTILE_DISC("v", 0.5, ORDMASK), 0, "PERCENTILE_DISC honours the mask");

    /* q is REFUSED, not answered: a NaN q would silently return the minimum
       under `q < 0` comparisons and an out-of-range q would clamp, and both
       are plausible-looking numbers produced from a caller error */
    throwsLike(() => df.QUANTILE("v", 1.5), "[0, 1]", "quantile(1.5) throws");
    throwsLike(() => df.QUANTILE("v", -0.5), "[0, 1]", "quantile(-0.5) throws");
    throwsLike(() => df.QUANTILE("v", NaN), "[0, 1]", "quantile(NaN) throws");
    throwsLike(() => df.QUANTILE("v"), "[0, 1]", "a missing q throws rather than defaulting");
    throwsLike(() => df.PERCENTILE_CONT("v", 2), "PERCENTILE_CONT",
               "PERCENTILE_CONT names ITSELF in its error, not its alias");
    throwsLike(() => df.PERCENTILE_DISC("v", 2), "PERCENTILE_DISC",
               "PERCENTILE_DISC names itself");
    throwsLike(() => new DataFrame({ s: ["a"] }).MEDIAN("s"), "string column",
               "median refuses a string column");
    throwsLike(() => new DataFrame({ s: ["a"] }).PERCENTILE_CONT("s", 0.5), "string column",
               "PERCENTILE_CONT refuses a string column");

    /* the whole family against the definitions, everywhere */
    let cells = 0, bad = 0;
    for (const [tag, T] of NUMERIC) {
        for (const n of N_TAILS) {
            const a = build(T, GEN[tag], n);
            const vals = arr(a), m = maskFor(n);
            const d = new DataFrame({ a });
            for (const mask of [undefined, m]) {
                const wantMed = medianByDefinition(vals, mask);
                const gotMed = d.MEDIAN("a", mask);
                const agree = (x, y) => x === y || (Number.isNaN(x) && Number.isNaN(y));
                if (!agree(gotMed, wantMed)) bad++;
                else if (!agree(gotMed, d.PERCENTILE_CONT("a", 0.5, mask))) bad++;
                else if (!agree(gotMed, d.QUANTILE("a", 0.5, mask))) bad++;
                else for (const q of [0, 0.1, 0.25, 0.5, 0.75, 0.9, 1]) {
                    if (!agree(d.PERCENTILE_DISC("a", q, mask),
                               discByDefinition(vals, mask, q))) { bad++; break; }
                }
                cells++;
            }
        }
    }
    ok(bad === 0, "median/quantile/PERCENTILE_CONT/PERCENTILE_DISC match the definitions",
       bad + " of " + cells + " cells disagree");
    mark("MEDIAN", "QUANTILE", "PERCENTILE_CONT", "PERCENTILE_DISC");
}

S("nlargest / nsmallest");
{
    const df = new DataFrame({ v: ORD });
    elemEq(df.N_LARGEST("v", 3), [3, 3, 1], "nlargest(3): descending");
    elemEq(df.N_SMALLEST("v", 3), [-5, -0, 0], "nsmallest(3): ascending");
    ok(df.N_LARGEST("v", 3) instanceof Float64Array, "nlargest returns a Float64Array");

    /* NaN is not an extreme, so it is EXCLUDED -- which means the result can be
       SHORTER than k. sort keeps it instead, because sort is a permutation. */
    eq(df.N_LARGEST("v", 100).length, 6, "nlargest(100) is 6 long, not 7: the NaN is dropped");
    eq(df.SORT("v").length, 7, "...while sort, a permutation, keeps it");
    eq(new DataFrame({ v: new Float64Array([NaN, NaN]) }).N_LARGEST("v", 2).length, 0,
       "nlargest of an all-NaN column is EMPTY");
    elemEq(df.N_LARGEST("v", 100), [3, 3, 1, -0, 0, -5], "nlargest(k > n) clamps");
    eq(df.N_LARGEST("v", 0).length, 0, "nlargest(0) is empty, not an error");

    /* the link to the methods already shipped */
    for (const [tag, T] of NUMERIC) {
        const a = build(T, GEN[tag], 25);
        const d = new DataFrame({ a });
        eq(d.N_LARGEST("a", 1)[0], d.MAX("a"), "nlargest(1) is max over " + tag);
        eq(d.N_SMALLEST("a", 1)[0], d.MIN("a"), "nsmallest(1) is min over " + tag);
    }

    eq(new DataFrame({ v: new Float64Array(0) }).N_SMALLEST("v", 3).length, 0,
       "nsmallest of an empty frame");
    elemEq(df.N_LARGEST("v", 3, ORDMASK), [3, 1, 0], "nlargest honours the mask");
    elemEq(df.N_SMALLEST("v", 2, ORDMASK), [-5, 0], "nsmallest honours the mask");

    /* k is REFUSED, not truncated: nlargest(col, 2.7) silently meaning 2 is a
       wrong answer the caller cannot see */
    throwsLike(() => df.N_LARGEST("v", 2.5), "non-negative integer", "nlargest(2.5) throws");
    throwsLike(() => df.N_LARGEST("v", -1), "non-negative integer", "nlargest(-1) throws");
    throwsLike(() => df.N_LARGEST("v", Infinity), "non-negative integer", "nlargest(Infinity) throws");
    throwsLike(() => df.N_LARGEST("v", NaN), "non-negative integer", "nlargest(NaN) throws");
    throwsLike(() => df.N_LARGEST("v"), "non-negative integer", "a missing k throws");
    throwsLike(() => df.N_SMALLEST("v", -1), "N_SMALLEST", "nsmallest names itself in its error");
    throwsLike(() => new DataFrame({ s: ["a"] }).N_LARGEST("s", 1), "string column",
               "nlargest refuses a string column");

    let cells = 0, bad = 0;
    for (const [tag, T] of NUMERIC) {
        for (const n of N_TAILS) {
            const a = build(T, GEN[tag], n);
            const vals = arr(a), m = maskFor(n);
            const d = new DataFrame({ a });
            for (const mask of [undefined, m]) {
                const asc = refValued(vals, mask);
                for (const k of [0, 1, 3, n, n + 5]) {
                    const kk = Math.min(k, asc.length);
                    const wantSmall = asc.slice(0, kk);
                    const wantLarge = asc.slice(asc.length - kk).reverse();
                    const gs = arr(d.N_SMALLEST("a", k, mask));
                    const gl = arr(d.N_LARGEST("a", k, mask));
                    if (gs.length !== kk || gs.some((x, i) => x !== wantSmall[i])) bad++;
                    else if (gl.length !== kk || gl.some((x, i) => x !== wantLarge[i])) bad++;
                    cells++;
                }
            }
        }
    }
    ok(bad === 0, "nlargest/nsmallest are the ends of the sorted order, everywhere",
       bad + " of " + cells + " cells disagree");
    mark("N_LARGEST", "N_SMALLEST");
}

/* ------------------------------------------------- ordering: memory safety
   These belong with the attacks above and use the same helper. Each one hooks
   the coercion the method ACTUALLY performs: a q argument is reached through
   valueOf, never toString, so an attack written against toString here would
   pass having run nothing. */
{
    attack("detach the column buffer during quantile's q coercion (valueOf)", (fire) => {
        const { df, buf } = hostile();
        return df.QUANTILE("v", { valueOf() { fire(); buf.transfer(); return 0.5; } });
    });
}
{
    attack("detach the column buffer during nlargest's k coercion (valueOf)", (fire) => {
        const { df, buf } = hostile();
        return df.N_LARGEST("v", { valueOf() { fire(); buf.transfer(); return 3; } });
    });
}
{
    attack("detach the MASK during median's column-name coercion (toString)", (fire) => {
        const mbuf = new ArrayBuffer(64);
        const mask = new Uint8Array(mbuf); mask.fill(1);
        const df = new DataFrame({ v: new Float64Array(64) });
        return df.MEDIAN({ toString() { fire(); mbuf.transfer(); return "v"; } }, mask);
    });
}
{
    attack("shrink a resizable buffer during argsort's column-name coercion", (fire) => {
        const rb = new ArrayBuffer(8 * 64, { maxByteLength: 8 * 64 });
        const df = new DataFrame({ v: new Float64Array(rb) });
        return df.ARG_SORT({ toString() { fire(); try { rb.resize(8); } catch (e) {} return "v"; } });
    });
}
{   /* already detached before the call: every one of the nine must throw and
       must name the column, or the caller cannot tell which one is dead */
    const buf = new ArrayBuffer(8 * 64);
    const v = new Float64Array(buf);
    const df = new DataFrame({ v });
    buf.transfer();
    throwsLike(() => df.SORT("v"), "detached", "sort on an already-detached buffer");
    throwsLike(() => df.ARG_SORT("v"), "detached", "argsort on an already-detached buffer");
    throwsLike(() => df.RANK("v"), "detached", "rank on an already-detached buffer");
    throwsLike(() => df.MEDIAN("v"), "detached", "median on an already-detached buffer");
    throwsLike(() => df.QUANTILE("v", 0.5), "detached", "quantile on an already-detached buffer");
    throwsLike(() => df.PERCENTILE_CONT("v", 0.5), "detached", "PERCENTILE_CONT on a detached buffer");
    throwsLike(() => df.PERCENTILE_DISC("v", 0.5), "detached", "PERCENTILE_DISC on a detached buffer");
    throwsLike(() => df.N_LARGEST("v", 2), "detached", "nlargest on an already-detached buffer");
    throwsLike(() => df.N_SMALLEST("v", 2), "detached", "nsmallest on an already-detached buffer");
}
{   /* a detached MASK, present before the call */
    const mbuf = new ArrayBuffer(64);
    const mask = new Uint8Array(mbuf); mask.fill(1);
    const df = new DataFrame({ v: new Float64Array(64) });
    mbuf.transfer();
    throws(() => df.SORT("v", mask), "sort with an already-detached mask");
    throws(() => df.RANK("v", mask), "rank with an already-detached mask");
    throws(() => df.MEDIAN("v", mask), "median with an already-detached mask");
    throws(() => df.N_LARGEST("v", 2, mask), "nlargest with an already-detached mask");
}
{   /* the frame's own writes are visible: the module aliases, it does not copy */
    const v = new Float64Array([3, 1, 2]);
    const df = new DataFrame({ v });
    elemEq(df.SORT("v"), [1, 2, 3], "sort before mutation");
    v[0] = -1;
    elemEq(df.SORT("v"), [-1, 1, 2], "sort sees writes made through the original TypedArray");
}

/* ================================================ generated: g2 */

/* ================================================================== BLOCK A */

/* ============================================== scans: cumsum/cumprod/cummax/cummin */
S("scans");
{
    /* References are written from the DEFINITION -- a serial left-to-right fold
       -- and never by calling another method on the frame. A reference that
       shares code with the thing under test proves nothing.

       These are bit-exact comparisons for EVERY dtype, including float, because
       both sides fold in the same order. The reassociation question only
       appears further down, where a scan's last element is compared with the
       REDUCTION of the same column: that one folds through several independent
       accumulators and is equal only up to summation order. */
    const ID = { CUM_SUM: 0, CUM_PROD: 1, CUM_MAX: -Infinity, CUM_MIN: Infinity };
    const scanRef = (a, kind, m) => {
        const out = new Array(a.length);
        let acc = ID[kind];
        for (let i = 0; i < a.length; i++) {
            const x = (!m || m[i]) ? a[i] : ID[kind];
            if (kind === "CUM_SUM") acc += x;
            else if (kind === "CUM_PROD") acc *= x;
            else if (kind === "CUM_MAX") acc = x > acc ? x : acc;
            else acc = x < acc ? x : acc;
            out[i] = acc;
        }
        return out;
    };

    /* the shapes, spelled out once so a reader can see the contract without
       running the reference */
    const df = new DataFrame({ v: new Float64Array([1, 3, 2, 5, 4]) });
    elemEq(df.CUM_SUM("v"), [1, 4, 6, 11, 15], "CUM_SUM");
    elemEq(df.CUM_PROD("v"), [1, 3, 6, 30, 120], "CUM_PROD");
    elemEq(df.CUM_MAX("v"), [1, 3, 3, 5, 5], "CUM_MAX");
    elemEq(df.CUM_MIN("v"), [1, 1, 1, 1, 1], "CUM_MIN");

    /* Every dtype x every tail length, masked and not. Two generators, because
       one of them cannot exercise both families: GENP's small factors are what
       keep cumprod finite, and GEN's wide values are what make cumsum and
       cummax do any work at all. */
    let cells = 0;
    for (const [tag, T] of NUMERIC) {
        for (const n of N_TAILS) {
            const p = build(T, GENP[tag], n);   /* small factors: cumprod stays finite */
            const a = build(T, GEN[tag], n);    /* the full range of the type */
            const m = maskFor(n);
            const d = new DataFrame({ p, a });
            const at = tag + "[" + n + "]";
            cells++;
            for (const kind of ["CUM_SUM", "CUM_PROD", "CUM_MAX", "CUM_MIN"]) {
                elemEq(d[kind]("p"), scanRef(p, kind), kind + " " + at);
                elemEq(d[kind]("p", m), scanRef(p, kind, m), kind + " masked " + at);
                eq(d[kind]("p").length, n, kind + " is nrows long " + at);
                eq(d[kind]("p", m).length, n,
                   kind + " masked is STILL nrows long (no compaction) " + at);
            }
            for (const kind of ["CUM_SUM", "CUM_MAX", "CUM_MIN"]) {
                elemEq(d[kind]("a"), scanRef(a, kind), kind + " wide " + at);
                elemEq(d[kind]("a", m), scanRef(a, kind, m), kind + " wide masked " + at);
            }
        }
    }
    ok(cells === NUMERIC.length * N_TAILS.length, "scan sweep covered every dtype x length",
       cells + " cells");
    mark("CUM_SUM", "CUM_PROD", "CUM_MAX", "CUM_MIN");
}

/* =========================================== the scan/reduction identity */
S("scan last element === the reduction");
{
    /* The property the whole design rests on: element i of a scan is the same
       aggregate over rows 0..i, so its LAST element is the reduction.
       The arithmetic differs by family and the assertions differ with it:
         - cummax/cummin are order-independent, so EXACT.
         - cumsum/cumprod on an INTEGER column are exact HERE because the data
           keeps every partial sum far below 2^53; the reduction accumulates in
           int64 and the scan in double, and they agree only while that holds.
         - cumsum/cumprod on a FLOAT column are equal only UP TO SUMMATION
           ORDER: the reduction folds through several independent accumulators
           and reassociates, a scan is serial and cannot. `near`, never `eq`. */
    for (const [tag, T] of NUMERIC) {
        const isInt = !!NUMERIC.find((t) => t[0] === tag)[2];
        for (const n of [1, 7, 16, 17, 33]) {
            const a = build(T, GENP[tag], n);
            const m = maskFor(n);
            const d = new DataFrame({ a });
            const at = tag + "[" + n + "]";
            const last = (x) => x[x.length - 1];
            if (isInt) {
                eq(last(d.CUM_SUM("a")), d.SUM("a"), "cumsum[n-1] === sum " + at);
                eq(last(d.CUM_PROD("a")), d.PRODUCT("a"), "cumprod[n-1] === product " + at);
            } else {
                near(last(d.CUM_SUM("a")), d.SUM("a"), "cumsum[n-1] ~= sum " + at, 1e-12);
                near(last(d.CUM_PROD("a")), d.PRODUCT("a"), "cumprod[n-1] ~= product " + at, 1e-12);
            }
            eq(last(d.CUM_MAX("a")), d.MAX("a"), "cummax[n-1] === max " + at);
            eq(last(d.CUM_MIN("a")), d.MIN("a"), "cummin[n-1] === min " + at);
            /* and with a mask, which is the case the NaN-slot reading would
               break whenever the LAST row is masked out */
            if (isInt) eq(last(d.CUM_SUM("a", m)), d.SUM("a", m), "masked cumsum[n-1] === masked sum " + at);
            else near(last(d.CUM_SUM("a", m)), d.SUM("a", m), "masked cumsum[n-1] ~= masked sum " + at, 1e-12);
            /* min/max return UNDEFINED when the mask selects nothing; a column
               cannot hold undefined, so the scan carries the seed instead. That
               asymmetry is the contract, and it is why this compares only when
               something is selected. */
            if (d.COUNT("a", m) > 0) {
                eq(last(d.CUM_MAX("a", m)), d.MAX("a", m), "masked cummax[n-1] === masked max " + at);
                eq(last(d.CUM_MIN("a", m)), d.MIN("a", m), "masked cummin[n-1] === masked min " + at);
            }
        }
    }
}

/* ================================================ scan value contract */
S("scan NaN and mask contract");
{
    for (const n of [5, 16, 17]) {
        /* cumsum/cumprod PROPAGATE NaN from its index onward; cummax/cummin
           IGNORE it, exactly as sum/product and min/max already do. */
        const v = new Float64Array(n).fill(2); v[2] = NaN;
        const d = new DataFrame({ v });
        const cs = arr(d.CUM_SUM("v")), cx = arr(d.CUM_MAX("v"));
        ok(cs.slice(0, 2).every(Number.isFinite), "cumsum before the NaN is finite [" + n + "]");
        ok(cs.slice(2).every(Number.isNaN), "cumsum is NaN from the NaN onward [" + n + "]");
        ok(cx.every((x) => x === 2), "cummax IGNORES the NaN entirely [" + n + "]");

        /* an all-NaN column surfaces the SEED, which is exactly what max() and
           min() return for it. NOT NaN: that is the pandas skipna reading and
           it would break cummax[n-1] === max. */
        const nan = new Float64Array(n).fill(NaN);
        const dn = new DataFrame({ nan });
        elemEq(dn.CUM_MAX("nan"), new Array(n).fill(-Infinity),
               "all-NaN cummax -> the seed -Infinity [" + n + "]");
        elemEq(dn.CUM_MIN("nan"), new Array(n).fill(Infinity),
               "all-NaN cummin -> the seed +Infinity [" + n + "]");
        eq(dn.CUM_MAX("nan")[n - 1], dn.MAX("nan"), "and it agrees with max() [" + n + "]");
        elemEq(dn.CUM_SUM("nan"), new Array(n).fill(NaN), "all-NaN cumsum -> NaN [" + n + "]");
    }

    /* THE MASKED-SCAN DECISION, spelled out. A masked-out element folds in the
       IDENTITY -- the reduction's own rule -- so it leaves the running value
       untouched and its slot carries that running value. Length is unchanged.
       Two alternatives are excluded here: a NaN slot (pandas skipna) and a
       COMPACTED result. */
    const v = new Float64Array([1, Infinity, 5, -Infinity, 2]);
    const d = new DataFrame({ v });
    const m = new Uint8Array([1, 0, 1, 0, 1]);
    elemEq(d.CUM_SUM("v", m), [1, 1, 6, 6, 8], "masked cumsum: identity folded, slot = running total");
    elemEq(d.CUM_MAX("v", m), [1, 1, 5, 5, 5], "masked cummax excludes the +Infinity");
    elemEq(d.CUM_MIN("v", m), [1, 1, 1, 1, 1], "masked cummin excludes the -Infinity");
    eq(d.CUM_SUM("v", m).length, 5, "the masked result is NOT compacted");
    ok(arr(d.CUM_SUM("v", m)).every(Number.isFinite),
       "a masked-out Infinity does not poison the running total");
    eq(d.CUM_SUM("v", m)[4], d.SUM("v", m), "and the last element is the masked sum");

    /* a masked-out row 0 has no running value yet, so its slot IS the identity */
    const d2 = new DataFrame({ v: new Float64Array([7, 2, 3]) });
    const m2 = new Uint8Array([0, 1, 1]);
    elemEq(d2.CUM_SUM("v", m2), [0, 2, 5], "masked-out row 0: cumsum slot is 0, the identity");
    elemEq(d2.CUM_PROD("v", m2), [1, 2, 6], "masked-out row 0: cumprod slot is 1, the identity");
    elemEq(d2.CUM_MAX("v", m2), [-Infinity, 2, 3], "masked-out row 0: cummax slot is the seed");

    /* an all-zero mask selects nothing: every slot is the identity. The
       reduction answers `undefined` for min/max here and a column cannot, so
       the scan carries the seed -- pinned so the asymmetry is deliberate. */
    const z = new Uint8Array(3);
    elemEq(d2.CUM_SUM("v", z), [0, 0, 0], "all-zero mask: cumsum is all 0");
    elemEq(d2.CUM_PROD("v", z), [1, 1, 1], "all-zero mask: cumprod is all 1");
    elemEq(d2.CUM_MAX("v", z), [-Infinity, -Infinity, -Infinity],
           "all-zero mask: cummax is the seed, where max() answers undefined");
    eq(d2.MAX("v", z), undefined, "the reduction it deliberately disagrees with");

    /* mask rules are the shared ones: truthiness not equality, longer is fine,
       shorter is refused, a plain Array is not a mask */
    elemEq(d2.CUM_SUM("v", new Uint8Array([0, 255, 2])), [0, 2, 5],
           "any nonzero mask byte selects");
    elemEq(d2.CUM_SUM("v", new Uint8Array(9).fill(1)), [7, 9, 12],
           "a mask longer than the column is accepted");
    throwsLike(() => d2.CUM_SUM("v", new Uint8Array(2)), "at least", "a short mask is refused");
    throws(() => d2.CUM_SUM("v", [1, 1, 1]), "a plain Array is not a mask");
    /* a Uint8Array column aliased as its own mask: nothing is written through
       either pointer, so this is legal */
    const u = new Uint8Array([1, 0, 3]);
    elemEq(new DataFrame({ u }).CUM_SUM("u", u), [1, 1, 4], "a column aliased as its own mask");

    /* the sign of zero is NOT pinned: a serial scan is deterministic today but
       a parallel one need not agree, exactly as for the min/max reduction */
    const zz = new DataFrame({ v: new Float64Array([-0, 0]) });
    eq(zz.CUM_MAX("v")[1], 0, "cummax over -0 and +0 is zero (sign unspecified)");
    eq(zz.CUM_SUM("v")[1], 0, "cumsum over -0 and +0 is zero");
}

/* ========================================================= shift and diff */
S("shift and diff");
{
    const shiftRef = (a, k, isDiff) => {
        const n = a.length, out = new Array(n);
        for (let i = 0; i < n; i++) {
            const j = i - k;
            const xj = (j >= 0 && j < n) ? a[j] : NaN;
            out[i] = isDiff ? a[i] - xj : xj;
        }
        return out;
    };
    const v = new Float64Array([10, 20, 30, 40]);
    const d = new DataFrame({ v });

    /* the slots with no source row are NaN and NEVER 0: zero reads as "no
       change", which is a plausible wrong answer, and the map family already
       uses NaN for a row it cannot compute */
    elemEq(d.SHIFT("v"), [NaN, 10, 20, 30], "shift defaults to 1 (a lag)");
    elemEq(d.SHIFT("v", 2), [NaN, NaN, 10, 20], "shift 2");
    elemEq(d.SHIFT("v", 0), [10, 20, 30, 40], "shift 0 is a copy");
    elemEq(d.SHIFT("v", -1), [20, 30, 40, NaN], "a NEGATIVE period is a lead");
    elemEq(d.DIFF("v"), [NaN, 10, 10, 10], "diff defaults to 1");
    elemEq(d.DIFF("v", -1), [-10, -10, -10, NaN], "diff with a negative period");
    elemEq(d.DIFF("v", 0), [0, 0, 0, 0], "diff 0 is all zero");

    /* diff(col, k) is LAG-k, x[i] - x[i-k]. numpy's iterated k-th difference
       would give [NaN, NaN, 0, 0] on this column; pandas' lag-k gives 20s. */
    elemEq(d.DIFF("v", 2), [NaN, NaN, 20, 20], "diff 2 is x[i]-x[i-2], not a 2nd difference");
    const g = new DataFrame({ v: new Float64Array([1, 2, 4, 8]) });
    elemEq(g.DIFF("v", 2), [NaN, NaN, 3, 6], "and again on a column where the two differ");

    /* a period at or past the column length puts every source row out of
       range. This must be a total answer and not a wrapped index: ToInt32 on
       1e10 is 1410065408, which would shift by a plausible wrong amount. */
    for (const p of [4, 5, 1e10, 2 ** 40, Infinity]) {
        elemEq(d.SHIFT("v", p), [NaN, NaN, NaN, NaN], "shift " + p + " is all NaN");
        elemEq(d.SHIFT("v", -p), [NaN, NaN, NaN, NaN], "shift -" + p + " is all NaN");
        elemEq(d.DIFF("v", p), [NaN, NaN, NaN, NaN], "diff " + p + " is all NaN");
    }

    /* THE IDENTITY, and it is exact: element-wise, no reassociation anywhere */
    for (const k of [-3, -2, -1, 0, 1, 2, 3]) {
        const sh = d.SHIFT("v", k), dd = d.DIFF("v", k), want = [];
        for (let i = 0; i < 4; i++) want.push(v[i] - sh[i]);
        elemEq(dd, want, "diff(col," + k + ") === sub(col, shift(col," + k + "))");
        elemEq(dd, arr(new DataFrame({ v, s: Float64Array.from(sh) }).SUB("v", "s")),
               "and it agrees with sub() itself, k=" + k);
    }

    /* every dtype x every tail length, both directions */
    for (const [tag, T] of NUMERIC) {
        for (const n of N_TAILS) {
            const a = build(T, GEN[tag], n);
            const dt = new DataFrame({ a });
            const at = tag + "[" + n + "]";
            for (const k of [-2, -1, 0, 1, 2, 5]) {
                elemEq(dt.SHIFT("a", k), shiftRef(a, k, false), "shift " + k + " " + at);
                elemEq(dt.DIFF("a", k), shiftRef(a, k, true), "diff " + k + " " + at);
            }
            eq(dt.SHIFT("a").length, n, "shift is nrows long " + at);
            eq(dt.DIFF("a").length, n, "diff is nrows long " + at);
        }
    }

    /* NaN in the data is ordinary arithmetic here: shift MOVES it, diff
       propagates it into both neighbours */
    const nd = new DataFrame({ v: new Float64Array([1, NaN, 3]) });
    elemEq(nd.SHIFT("v", 1), [NaN, 1, NaN], "shift moves a NaN");
    elemEq(nd.DIFF("v", 1), [NaN, NaN, NaN], "diff propagates a NaN both ways");

    /* a fractional or NaN period is REFUSED rather than truncated: the caller
       asked a question that has no answer, and truncating answers a different
       one. The message names the method, or the caller cannot tell which call
       is wrong. */
    throwsLike(() => d.SHIFT("v", 1.5), "integer", "shift refuses a fractional period");
    throwsLike(() => d.SHIFT("v", 1.5), "SHIFT", "and names the method");
    throwsLike(() => d.DIFF("v", -0.5), "integer", "diff refuses a fractional period");
    throwsLike(() => d.DIFF("v", NaN), "integer", "diff refuses a NaN period");
    throwsLike(() => d.SHIFT("v", "x"), "integer", "a non-numeric period coerces to NaN and is refused");
    /* undefined means "use the default", not "coerce to NaN" */
    elemEq(d.SHIFT("v", undefined), [NaN, 10, 20, 30], "an undefined period means the default 1");
    /* -0 is an integer and is the same shift as 0 */
    elemEq(d.SHIFT("v", -0), [10, 20, 30, 40], "-0 is a legal period");

    /* diff/shift take NO mask: they are the element-wise map family, and
       nothing in it does. A third argument is ignored, exactly as an extra
       argument is ignored everywhere else in this module. */
    elemEq(d.SHIFT("v", 1, new Uint8Array([1, 0, 1, 0])), [NaN, 10, 20, 30],
           "shift ignores a third argument; it has no mask");

    mark("DIFF", "SHIFT");
}

/* ============================================ scans: empty and string columns */
S("scan edges");
{
    for (const [tag, T] of NUMERIC) {
        const e = new DataFrame({ a: new T(0) });
        for (const k of ["CUM_SUM", "CUM_PROD", "CUM_MAX", "CUM_MIN", "DIFF", "SHIFT"])
            eq(e[k]("a").length, 0, "empty " + tag + " " + k + " -> length 0");
    }
    const s = new DataFrame({ a: [1, 2, 3] });
    for (const k of ["CUM_SUM", "CUM_PROD", "CUM_MAX", "CUM_MIN", "DIFF", "SHIFT"])
        throwsLike(() => s[k]("a"), "string column", k + " refuses a string column");
    /* the refusal names the method that refused */
    throwsLike(() => s.CUM_SUM("a"), "CUM_SUM", "and names cumsum");
    throwsLike(() => s.CUM_MIN("a"), "CUM_MIN", "and names cummin");
    throwsLike(() => s.DIFF("a"), "DIFF", "and names diff");
    for (const k of ["CUM_SUM", "CUM_PROD", "CUM_MAX", "CUM_MIN", "DIFF", "SHIFT"])
        throwsLike(() => new DataFrame({ a: new Float64Array(1) })[k]("nope"),
                   "no such column", k + " on an unknown column");
    /* a subarray column is honoured, like everywhere else */
    elemEq(new DataFrame({ a: new Float64Array([1, 2, 3, 4]).subarray(1, 3) }).CUM_SUM("a"),
           [2, 5], "subarray column respects byteOffset");
}

/* ================================================================== BLOCK B
   Two attacks, to be pasted after attack 10 and BEFORE `ok(hooksFired >= 10`.
   THAT LINE MUST BECOME `ok(hooksFired >= 12, ...)`.
   The coercion map is the one at the top of that region: a column NAME reaches
   toString, a NUMBER reaches valueOf, a mask reaches neither. */

{   /* 11. shift's PERIODS argument is the only new coercion these six add, and
       it is a valueOf. Written against toString it would run nothing. */
    attack("detach the column buffer during shift's PERIODS coercion (valueOf)", (fire) => {
        const { df, buf } = hostile();
        return df.SHIFT("v", { valueOf() { fire(); buf.transfer(); return 1; } });
    });
}
{   /* 12. cumsum takes the mask pointer AFTER the column name is coerced, so a
       detach from inside that coercion must be caught by the mask check and not
       by a read through a stale pointer. */
    attack("detach the MASK buffer during cumsum's column-name coercion", (fire) => {
        const mbuf = new ArrayBuffer(64);
        const mask = new Uint8Array(mbuf); mask.fill(1);
        const df = new DataFrame({ v: new Float64Array(64) });
        return df.CUM_SUM({ toString() { fire(); mbuf.transfer(); return "v"; } }, mask);
    });
}

/* ================================================================== BLOCK C
   To be pasted inside the existing "detached BEFORE the call" block, beside the
   other throwsLike(..., "detached", ...) lines. */

    throwsLike(() => df.CUM_SUM("v"), "detached", "cumsum on an already-detached buffer");
    throwsLike(() => df.CUM_PROD("v"), "detached", "cumprod on an already-detached buffer");
    throwsLike(() => df.CUM_MAX("v"), "detached", "cummax on an already-detached buffer");
    throwsLike(() => df.CUM_MIN("v"), "detached", "cummin on an already-detached buffer");
    throwsLike(() => df.DIFF("v"), "detached", "diff on an already-detached buffer");
    throwsLike(() => df.SHIFT("v"), "detached", "shift on an already-detached buffer");
    throws(() => df.CUM_SUM("v", new Uint8Array(64)), "masked cumsum on a detached column");

/* ================================================ generated: g4 */

/* References, written from the DEFINITION rather than from another optimised
   path. `better` is a STRICT compare, which is what makes the first occurrence
   win a tie; passing <= here instead of < is the tie bug, and the randomised
   block below reports how many columns would expose it. */
function refRows(mask, n) {
    const out = [];
    for (let i = 0; i < n; i++) if (!mask || mask[i]) out.push(i);
    return out;
}
function refArg(col, mask, n, better) {
    let at = -1;
    for (let i = 0; i < n; i++) {
        if (mask && !mask[i]) continue;
        const v = col[i];
        if (Number.isNaN(v)) continue;          /* min/max skip it, so we do */
        if (at < 0 || better(v, col[at])) at = i;   /* SEED, then compare */
    }
    return at;
}
const LESS = (a, b) => a < b, MORE = (a, b) => a > b;

/* THE FOUR INJECTED FAULTS.
 *
 * A reference that agrees with the kernel proves the kernel matches the
 * reference; it does not prove the CORPUS could have seen a bug. These four
 * are the wrong implementations the C deliberately avoids, written out here so
 * the randomised block can count how many of its columns each one gets wrong.
 * If any count is zero the corpus has drifted out of range of that fault and
 * the assertion that it is REACHABLE fails -- which is the only thing that
 * makes the agreement above worth anything. Each was also injected into the C
 * itself during development and made the oracle fail; these keep that check
 * runnable against the shipped binary, where the C cannot be edited.
 *
 *   1 SENTINEL  argmin seeded with the +Infinity identity instead of with the
 *               first candidate: answers "none" for an all-+Infinity column.
 *   2 TIE_LAST  <= instead of <: the LAST occurrence of a tie wins.
 *   3 NAN_KEPT  NaN not skipped: it seeds, and nothing ever beats it, so the
 *               answer is the index of the first NaN.
 *   4 TAIL_HEAD tail that forgets to skip the leading rows -- i.e. head. */
function bugSentinel(col, mask, n) {
    let at = -1, best = Infinity;
    for (let i = 0; i < n; i++) {
        if (mask && !mask[i]) continue;
        if (Number.isNaN(col[i])) continue;
        if (col[i] < best) { best = col[i]; at = i; }
    }
    return at;
}
function bugTieLast(col, mask, n) {
    let at = -1;
    for (let i = 0; i < n; i++) {
        if (mask && !mask[i]) continue;
        if (Number.isNaN(col[i])) continue;
        if (at < 0 || col[i] <= col[at]) at = i;
    }
    return at;
}
function bugNanKept(col, mask, n) {
    let at = -1;
    for (let i = 0; i < n; i++) {
        if (mask && !mask[i]) continue;
        if (at < 0 || col[i] < col[at]) at = i;
    }
    return at;
}
function bugTailIsHead(rows, k) { return rows.slice(0, Math.min(k, rows.length)); }

/* ================================================ positional: head/tail shape */
S("positional: head/tail shape");
{
    const c = new Float64Array([10, 11, 12, 13, 14, 15, 16]);
    const df = new DataFrame({ c });

    /* The default is a PEEK, not the whole column. If it were "everything",
       head() on a million rows would copy a million rows by accident. */
    eq(df.HEAD("c").length, 5, "head() defaults to 5 rows");
    elemEq(df.HEAD("c"), [10, 11, 12, 13, 14], "head() values");
    eq(df.TAIL("c").length, 5, "tail() defaults to 5 rows");
    /* tail does NOT reverse: head(k) ++ tail(rows-k) reconstructs the column,
       which is false the moment tail hands back descending order. */
    elemEq(df.TAIL("c"), [12, 13, 14, 15, 16], "tail() is ASCENDING, not reversed");
    elemEq(arr(df.HEAD("c", 3)).concat(arr(df.TAIL("c", 4))), arr(c),
           "head(3) ++ tail(4) reconstructs the column");

    eq(df.HEAD("c", 0).length, 0, "head(0) is empty -- 0 is a count, not 'omitted'");
    eq(df.TAIL("c", 0).length, 0, "tail(0) is empty");
    eq(df.HEAD("c", 1000).length, 7, "n > rows CLAMPS rather than throwing");
    eq(df.TAIL("c", 1000).length, 7, "tail clamps too");
    elemEq(df.TAIL("c", 1000), arr(c), "and clamping keeps row order");
    eq(df.HEAD("c", Infinity).length, 7, "n = Infinity clamps (never converted to an int)");
    eq(df.HEAD("c", 2.9).length, 2, "a fractional n truncates toward zero");
    eq(df.HEAD("c", null).length, 5, "null n means omitted, as it does for a mask");

    /* A NEGATIVE n is pandas' "all but the last n". That meaning is real but
       nobody types it by accident, whereas a negative count arriving here is
       usually a bug at the call site -- so it is REFUSED. This assertion is
       what fails if the decision is ever reversed to the pandas reading. */
    throwsLike(() => df.HEAD("c", -1), "non-negative", "head(-1) is refused, not read as 'all but the last'");
    throwsLike(() => df.TAIL("c", -1), "non-negative", "tail(-1) likewise");
    /* NaN is a coercion that failed quietly; returning an empty array for it
       would be indistinguishable downstream from an empty selection. */
    throwsLike(() => df.HEAD("c", NaN), "non-negative", "head(NaN) is refused, not treated as 0");
    throwsLike(() => df.HEAD("c", "abc"), "non-negative", "a string that is not a number is refused");
    eq(df.HEAD("c", "3").length, 3, "a numeric string still coerces, like every other numeric argument here");

    const empty = new DataFrame({ c: new Float64Array(0) });
    eq(empty.HEAD("c").length, 0, "head of an empty column is empty, not an error");
    eq(empty.TAIL("c").length, 0, "tail of an empty column is empty");
    mark("HEAD", "TAIL");
}

/* ================================== positional: head/tail COPY, never a view */
S("positional: head/tail copy");
{
    const c = new Float64Array([1, 2, 3, 4, 5]);
    const df = new DataFrame({ c });
    const h = df.HEAD("c", 3), t = df.TAIL("c", 3);

    /* The module aliases the caller's buffer for READING only; nothing native
       escapes into a JS value. A view would also make the result track later
       writes, which no other method here does. */
    ok(h.buffer !== c.buffer, "head's result does not alias the column's ArrayBuffer");
    ok(t.buffer !== c.buffer, "tail's result does not alias it either");
    c[0] = 99; c[4] = -99;
    eq(h[0], 1, "head COPIED: writing the column afterwards does not change the result");
    eq(t[2], 5, "tail COPIED");
    c[0] = 1; c[4] = 5;

    /* Float64Array for EVERY column type, exactly as the maps do. This is what
       makes a view impossible for seven of the eight dtypes, which is the
       reason copying is the consistent choice rather than a cost. */
    for (const [tag, T] of NUMERIC) {
        const d = new DataFrame({ v: build(T, GEN[tag], 4) });
        ok(d.HEAD("v", 2) instanceof Float64Array, tag + ": head returns a Float64Array");
        ok(d.TAIL("v", 2) instanceof Float64Array, tag + ": tail returns a Float64Array");
    }
    mark("HEAD", "TAIL");
}

/* ================================================ positional: masks */
S("positional: head/tail under a mask");
{
    const c = new Float64Array([0, 1, 2, 3, 4, 5, 6, 7]);
    const m = new Uint8Array([0, 1, 1, 0, 1, 0, 1, 1]);      /* 5 selected */
    const df = new DataFrame({ c });

    /* EXCLUDE, never zero -- the same rule the masked reductions follow. A
       kernel that multiplied by the mask would return 0 for row 0 here. */
    elemEq(df.HEAD("c", 3, m), [1, 2, 4], "head skips masked-out rows rather than zeroing them");
    elemEq(df.TAIL("c", 3, m), [4, 6, 7], "tail takes the LAST 3 selected, still ascending");
    elemEq(df.HEAD("c", 99, m), [1, 2, 4, 6, 7], "head(rows, mask) is the whole selection");
    elemEq(df.TAIL("c", 99, m), [1, 2, 4, 6, 7], "and so is tail(rows, mask)");

    /* Cross-check against a method that was verified before this one existed. */
    eq(df.HEAD("c", df.ROWS, m).length, df.COUNT("c", m),
       "head(rows, mask).length === count(mask)");
    eq(df.TAIL("c", df.ROWS, m).length, df.COUNT("c", m),
       "tail(rows, mask).length === count(mask)");

    const none = new Uint8Array(8);
    eq(df.HEAD("c", 3, none).length, 0, "a mask selecting nothing gives an empty head");
    eq(df.TAIL("c", 3, none).length, 0, "and an empty tail");
    const all = new Uint8Array(8).fill(1);
    elemEq(df.HEAD("c", 3, all), arr(df.HEAD("c", 3)), "an all-ones mask matches no mask at all");
    elemEq(df.TAIL("c", 3, all), arr(df.TAIL("c", 3)), "same for tail");

    throwsLike(() => df.HEAD("c", 3, new Uint8Array(2)), "at least",
               "a mask shorter than the frame is refused");
    mark("HEAD", "TAIL", "COUNT");
}

/* ================================================ positional: first/last */
S("positional: first/last");
{
    const c = new Float64Array([NaN, 1, 2, NaN]);
    const df = new DataFrame({ c });

    /* POSITIONAL, not an aggregate: first is "the value in the first selected
       row", so a NaN there comes back as NaN. Skipping it would break the
       identity with head(1) below, which is the whole reason first/last are
       defined this way. */
    eq(df.FIRST("c"), NaN, "first is positional: a NaN row is returned, not skipped");
    eq(df.LAST("c"), NaN, "last likewise");
    eq(df.FIRST("c"), df.HEAD("c", 1)[0], "first(c) === head(c, 1)[0]");
    eq(df.LAST("c"), df.TAIL("c", 1)[0], "last(c) === tail(c, 1)[0]");

    const m = new Uint8Array([0, 1, 1, 0]);
    eq(df.FIRST("c", m), 1, "first respects the mask");
    eq(df.LAST("c", m), 2, "last respects the mask");
    eq(df.FIRST("c", m), df.HEAD("c", 1, m)[0], "first(c, m) === head(c, 1, m)[0]");
    eq(df.LAST("c", m), df.TAIL("c", 1, m)[0], "last(c, m) === tail(c, 1, m)[0]");

    /* undefined, not NaN, for "there was no such row" -- NaN is a VALUE this
       method can legitimately return, so it cannot double as the empty answer.
       This is the same answer min and max already give for an empty column. */
    const none = new Uint8Array(4);
    eq(df.FIRST("c", none), undefined, "first of an empty selection is undefined, not NaN");
    eq(df.LAST("c", none), undefined, "last of an empty selection is undefined");
    const empty = new DataFrame({ c: new Float64Array(0) });
    eq(empty.FIRST("c"), undefined, "first of an empty column is undefined");
    eq(empty.LAST("c"), undefined, "last of an empty column is undefined");
    eq(empty.MIN("c"), undefined, "...which is what min already answers there");

    const one = new DataFrame({ c: new Float64Array([7]) });
    eq(one.FIRST("c"), 7, "a one-row column");
    eq(one.LAST("c"), 7, "first === last on one row");
    mark("FIRST", "LAST", "HEAD", "TAIL", "MIN");
}

/* ================================================ positional: argmin/argmax */
S("positional: argmin/argmax");
{
    const ties = new DataFrame({ c: new Float64Array([1, 0, 2, 0]) });
    /* FIRST occurrence. A <= in the kernel would answer 3 here and 0 for
       argmax, and nothing else in the suite would notice. */
    eq(ties.ARG_MIN("c"), 1, "argmin ties go to the FIRST occurrence");
    eq(ties.ARG_MAX("c"), 2, "argmax ties likewise");

    /* A column of +Infinity: min returns +Infinity and the column CONTAINS it,
       so there is an index. A kernel seeded with the +Infinity identity
       instead of with the first candidate answers undefined here -- correct on
       every column that does not contain +Infinity, which is why this row is
       the one that catches it. */
    const inf = new DataFrame({ c: new Float64Array([Infinity, Infinity]) });
    eq(inf.MIN("c"), Infinity, "min of an all-+Infinity column is +Infinity");
    eq(inf.ARG_MIN("c"), 0, "so argmin points at row 0 rather than answering undefined");

    /* NaN is skipped, exactly as min skips it. */
    const mix = new DataFrame({ c: new Float64Array([1, NaN, 5, -Infinity, 2]) });
    eq(mix.ARG_MIN("c"), 3, "argmin skips NaN, like min");
    eq(mix.ARG_MAX("c"), 2, "argmax skips NaN, like max");

    /* ABSOLUTE row index, not an index into the selection. Returning the
       position within the selected subset would give 1 here, and col[1] is the
       wrong value -- the identity below is what refuses that reading. */
    const m = new Uint8Array([0, 0, 1, 0, 1]);
    eq(mix.ARG_MIN("c", m), 4, "argmin under a mask returns the ABSOLUTE row index");
    eq(mix.ARG_MAX("c", m), 2, "argmax likewise");
    ok(m[mix.ARG_MIN("c", m)] !== 0, "and the row it names is a selected one");

    /* The no-witness cases. min still reports its identity for an all-NaN
       column, because a masked-out or NaN row folds the identity in; there is
       no index for that value, so argmin answers undefined. This is the ONE
       place min and col[argmin] are allowed to disagree, and it is pinned. */
    const nan3 = new DataFrame({ c: new Float64Array([NaN, NaN, NaN]) });
    eq(nan3.MIN("c"), Infinity, "min of an all-NaN column is the +Infinity identity");
    eq(nan3.MAX("c"), -Infinity, "max of an all-NaN column is the -Infinity identity");
    eq(nan3.ARG_MIN("c"), undefined, "argmin of an all-NaN column is undefined -- no row holds +Infinity");
    eq(nan3.ARG_MAX("c"), undefined, "argmax of an all-NaN column is undefined");
    eq(mix.ARG_MIN("c", new Uint8Array([0, 1, 0, 0, 0])), undefined,
       "a mask selecting only a NaN has no argmin");
    eq(mix.ARG_MIN("c", new Uint8Array(5)), undefined, "a mask selecting nothing has no argmin");
    const empty = new DataFrame({ c: new Float64Array(0) });
    eq(empty.ARG_MIN("c"), undefined, "an empty column has no argmin");
    eq(empty.ARG_MAX("c"), undefined, "an empty column has no argmax");

    /* An integer column cannot hold NaN, so there the rule collapses to
       "undefined exactly when nothing is selected". */
    const ints = new DataFrame({ c: new Int32Array([5, -3, 5, -3]) });
    eq(ints.ARG_MIN("c"), 1, "int column: first occurrence of the minimum");
    eq(ints.ARG_MAX("c"), 0, "int column: first occurrence of the maximum");
    eq(ints.ARG_MIN("c", new Uint8Array(4)), undefined, "int column, empty selection");
    ok(Number.isInteger(ints.ARG_MIN("c")), "argmin returns an integer Number, not a TypedArray");
    mark("ARG_MIN", "ARG_MAX", "MIN", "MAX");
}

/* =============================== positional: argmin AGREES with min (random) */
S("positional: argmin agrees with min (randomised)");
{
    /* THE test in this file. Every other assertion above is a hand-picked
       case, and a hand-picked case is chosen by the same person who wrote the
       kernel. This one is not: it draws columns from a pool built out of the
       values that make min and argmin disagree -- NaN, both infinities, both
       zeros, and repeats so ties are common -- and asserts the identity that
       has to hold for every one of them. */
    const POOL = [NaN, Infinity, -Infinity, 0, -0, 1, -1, 5, 5, 5,
                  2.5, -2.5, 1e308, -1e308, 3, 3];
    let rng = 2463534242 >>> 0;
    function xr() {
        let x = rng;
        x ^= x << 13; x >>>= 0;
        x ^= x >>> 17;
        x ^= x << 5;  x >>>= 0;
        rng = x; return x;
    }
    let witness = 0, noWitness = 0, tiesSeen = 0, infSeen = 0, bad_ = 0;
    let sawSentinel = 0, sawTieLast = 0, sawNanKept = 0, sawTailHead = 0;

    for (let t = 0; t < 3000 && bad_ === 0; t++) {
        const n = xr() % 41;
        const col = new Float64Array(n);
        const mk = new Uint8Array(n);
        for (let i = 0; i < n; i++) {
            col[i] = POOL[xr() % POOL.length];
            mk[i] = (xr() % 3 !== 0) ? 1 : 0;
        }
        const useMask = (xr() & 1) === 1;
        const m = useMask ? mk : undefined;
        const df = new DataFrame({ col });

        const ai = df.ARG_MIN("col", m), bi = df.ARG_MAX("col", m);
        const mn = df.MIN("col", m), mx = df.MAX("col", m);
        const ra = refArg(col, m, n, LESS), rb = refArg(col, m, n, MORE);

        /* count the columns on which each injected fault would give a
           DIFFERENT answer from the kernel: that count is the corpus's
           sensitivity to that fault, and it is asserted nonzero below */
        if (bugSentinel(col, m, n) !== ra) sawSentinel++;
        if (bugTieLast(col, m, n) !== ra) sawTieLast++;
        if (bugNanKept(col, m, n) !== ra) sawNanKept++;
        {
            const rows = refRows(m, n), k = 3;
            const want = rows.slice(Math.max(0, rows.length - k)).map((i) => col[i]);
            const wrong = bugTailIsHead(rows, k).map((i) => col[i]);
            const got = arr(df.TAIL("col", k, m));
            elemEq(got, want, "randomised tail(3)");
            if (String(wrong) !== String(want)) sawTailHead++;
        }

        /* the reference says -1 for "no candidate"; the method says undefined */
        const wantA = ra < 0 ? undefined : ra, wantB = rb < 0 ? undefined : rb;
        if (ai !== wantA || bi !== wantB) {
            bad_++;
            bad("randomised argmin/argmax", "trial " + t + " n=" + n +
                " got " + ai + "/" + bi + " want " + wantA + "/" + wantB +
                " col=[" + arr(col).join(",") + "]" +
                (useMask ? " mask=[" + arr(mk).join(",") + "]" : ""));
            continue;
        }
        pass++;

        if (ai === undefined) {
            noWitness++;
            /* the licensed disagreement, and ONLY here: nothing selected, or
               everything selected is NaN. min then reports either "no value"
               or the identity it folded for the rows it skipped. */
            ok(ra < 0, "no-witness case really has no candidate");
            ok(mn === undefined || mn === Infinity,
               "no-witness min is undefined or the +Infinity identity", String(mn));
            ok(mx === undefined || mx === -Infinity,
               "no-witness max is undefined or the -Infinity identity", String(mx));
        } else {
            witness++;
            /* === and not Object.is: the sign of zero is deliberately
               unspecified in min/max, and === does not separate the zeros. */
            eq(col[ai], mn, "col[argmin] === min");
            eq(col[bi], mx, "col[argmax] === max");
            ok(!m || m[ai] !== 0, "argmin names a SELECTED row");
            ok(!m || m[bi] !== 0, "argmax names a SELECTED row");
            /* first occurrence: no earlier selected row holds the same value */
            let earlier = -1;
            for (let i = 0; i < ai; i++)
                if ((!m || m[i]) && col[i] === col[ai]) { earlier = i; break; }
            ok(earlier < 0, "no earlier selected row ties the argmin", "row " + earlier);
            if (col.indexOf(col[ai]) !== col.lastIndexOf(col[ai])) tiesSeen++;
            if (col.indexOf(Infinity) >= 0 || col.indexOf(-Infinity) >= 0) infSeen++;
        }
    }
    /* A corpus that stops reaching a case proves nothing about it, and nothing
       links the pool to the kernel, so the reach is asserted rather than
       assumed. */
    ok(witness > 500, "REACHABLE: the corpus reaches the with-witness case", String(witness));
    ok(noWitness > 10, "REACHABLE: the corpus reaches the no-witness case", String(noWitness));
    ok(tiesSeen > 100, "REACHABLE: the corpus reaches tied minima", String(tiesSeen));
    ok(infSeen > 100, "REACHABLE: the corpus reaches columns containing an infinity", String(infSeen));
    /* The four faults, each proved VISIBLE to this corpus. A zero here means
       the pool drifted and the agreement above stopped being evidence. */
    /* The sentinel fault needs every selected non-NaN row to BE +Infinity, and
       the broad pool above reaches that only a handful of times in 3000 draws
       -- a floor that thin is one pool edit away from silently becoming zero.
       So it gets its own corpus, drawn from the two values that produce it. */
    {
        const NARROW = [Infinity, Infinity, NaN];
        for (let t = 0; t < 400; t++) {
            const n = 1 + (xr() % 6);
            const col = new Float64Array(n);
            const mk = new Uint8Array(n);
            for (let i = 0; i < n; i++) {
                col[i] = NARROW[xr() % NARROW.length];
                mk[i] = (xr() % 3 !== 0) ? 1 : 0;
            }
            const m = (xr() & 1) === 1 ? mk : undefined;
            const df = new DataFrame({ col });
            const ra = refArg(col, m, n, LESS);
            eq(df.ARG_MIN("col", m), ra < 0 ? undefined : ra, "narrow-corpus argmin");
            eq(df.ARG_MAX("col", m), ra < 0 ? undefined : ra,
               "narrow-corpus argmax (every finite candidate is +Infinity, so it is the same row)");
            if (ra >= 0) eq(col[ra], df.MIN("col", m), "narrow-corpus col[argmin] === min");
            if (bugSentinel(col, m, n) !== ra) sawSentinel++;
        }
    }
    ok(sawSentinel > 20, "REACHABLE: the identity-seeded argmin fault is visible to this corpus", String(sawSentinel));
    ok(sawTieLast > 0, "REACHABLE: the last-occurrence tie fault is visible", String(sawTieLast));
    ok(sawNanKept > 0, "REACHABLE: the NaN-not-ignored fault is visible", String(sawNanKept));
    ok(sawTailHead > 0, "REACHABLE: the tail-forgets-to-skip fault is visible", String(sawTailHead));
    console.log("  randomised: " + witness + " with a witness, " + noWitness +
                " without, " + tiesSeen + " tied, " + infSeen + " with an infinity");
    console.log("  injected faults visible on: sentinel " + sawSentinel +
                ", tie-last " + sawTieLast + ", nan-kept " + sawNanKept +
                ", tail-is-head " + sawTailHead + " columns");
    mark("ARG_MIN", "ARG_MAX", "MIN", "MAX");
}

/* ====================================== positional: dtype x tail-length sweep */
S("positional: dtype x length sweep");
{
    let cells = 0;
    for (const [tag, T] of NUMERIC) {
        for (const n of N_TAILS) {
            const a = build(T, GEN[tag], n);
            const m = maskFor(n);
            const df = new DataFrame({ a });
            const at = tag + "[" + n + "]";
            cells++;

            for (const mask of [undefined, m]) {
                const rows = refRows(mask, n);
                const label = at + (mask ? " masked" : "");
                for (const k of [0, 1, 3, n, n + 1]) {
                    const want = rows.slice(0, Math.min(k, rows.length)).map((i) => a[i]);
                    elemEq(df.HEAD("a", k, mask), want, label + " head(" + k + ")");
                    const tl = rows.slice(Math.max(0, rows.length - k)).map((i) => a[i]);
                    elemEq(df.TAIL("a", k, mask), tl, label + " tail(" + k + ")");
                }
                eq(df.FIRST("a", mask), rows.length ? a[rows[0]] : undefined, label + " first");
                eq(df.LAST("a", mask), rows.length ? a[rows[rows.length - 1]] : undefined, label + " last");

                const ra = refArg(a, mask, n, LESS), rb = refArg(a, mask, n, MORE);
                eq(df.ARG_MIN("a", mask), ra < 0 ? undefined : ra, label + " argmin");
                eq(df.ARG_MAX("a", mask), rb < 0 ? undefined : rb, label + " argmax");
                /* the identity again, now across every dtype and every tail
                   length rather than only on random f64 */
                if (ra >= 0) {
                    eq(a[ra], df.MIN("a", mask), label + " col[argmin] === min");
                    eq(a[rb], df.MAX("a", mask), label + " col[argmax] === max");
                } else {
                    eq(df.MIN("a", mask), undefined, label + " no candidate <=> min undefined (no NaN in these)");
                }
            }
        }
    }
    ok(cells === NUMERIC.length * N_TAILS.length, "sweep covered every dtype x length",
       String(cells));
    mark("HEAD", "TAIL", "FIRST", "LAST", "ARG_MIN", "ARG_MAX", "MIN", "MAX");
}

/* ============================ positional: string columns and bad arguments */
S("positional: refusals");
{
    const df = new DataFrame({ s: ["a", "b", "a"], v: new Float64Array([1, 2, 3]) });
    /* A string column is dictionary-encoded; handing back the CODES as numbers
       is a plausible wrong answer rather than something a caller would notice,
       so all six refuse it by name -- the same rule abs/round/clip follow. */
    for (const call of [() => df.HEAD("s"), () => df.TAIL("s"), () => df.FIRST("s"),
                        () => df.LAST("s"), () => df.ARG_MIN("s"), () => df.ARG_MAX("s")])
        throwsLike(call, "string column", "a string column is refused");

    for (const call of [() => df.HEAD("nope"), () => df.TAIL("nope"), () => df.FIRST("nope"),
                        () => df.LAST("nope"), () => df.ARG_MIN("nope"), () => df.ARG_MAX("nope")])
        throwsLike(call, "no such column", "an unknown column is refused");

    /* The refusal has to name the method, or a caller with six of these in one
       expression cannot tell which one threw. */
    throwsLike(() => df.HEAD("s"), "HEAD", "head names itself in its refusal");
    throwsLike(() => df.TAIL("s"), "TAIL", "tail names itself");
    throwsLike(() => df.FIRST("s"), "FIRST", "first names itself");
    throwsLike(() => df.LAST("s"), "LAST", "last names itself");
    throwsLike(() => df.ARG_MIN("s"), "ARG_MIN", "argmin names itself");
    throwsLike(() => df.ARG_MAX("s"), "ARG_MAX", "argmax names itself");
    mark("HEAD", "TAIL", "FIRST", "LAST", "ARG_MIN", "ARG_MAX");
}

/* ================================================ generated: g5 */

/* ============================================ moments: the closed forms */
S("moments: closed forms");
{
    const df = new DataFrame({ a: new Float64Array([1, 2, 3, 4]) });

    /* THE PIN. variance/stddev are SAMPLE (n-1); VARIANCE_POP/STDDEV_POP are the
       /n forms. If these two ever agree on this input, one of them changed
       divisor. Both numbers are exact in binary64, so this is `eq`, not `near`. */
    eq(df.VARIANCE("a"), 5 / 3, "variance stays SAMPLE (n-1)");
    eq(df.VARIANCE_POP("a"), 1.25, "VARIANCE_POP is POPULATION (/n)");
    eq(df.STDDEV_POP("a"), Math.sqrt(1.25), "STDDEV_POP == sqrt(VARIANCE_POP)");
    eq(df.STDDEV("a"), Math.sqrt(5 / 3), "stddev stays SAMPLE");

    /* ... and they are the SAME ALGORITHM, differing only by the divisor. A
       kernel that quietly switched to a one-pass or a Welford form would still
       satisfy the two lines above and fail this one. */
    near(df.VARIANCE_POP("a") * 4 / 3, df.VARIANCE("a"),
         "VARIANCE_POP * n/(n-1) IS the sample variance", 1e-15);

    eq(df.SKEW("a"), 0, "skew of a symmetric set is exactly 0");
    /* m2 = 1.25, m4 = 2.5625 -> 2.5625/1.5625 - 3 = -1.36 */
    near(df.KURTOSIS("a"), -1.36, "kurtosis is EXCESS (-3)", 1e-15);
    ok(Math.abs(df.KURTOSIS("a") - (2.5625 / 1.5625)) > 1,
       "kurtosis is NOT the raw fourth standardised moment",
       "got " + df.KURTOSIS("a") + ", raw would be " + (2.5625 / 1.5625));

    mark("VARIANCE_POP", "STDDEV_POP", "SKEW", "KURTOSIS");
}

/* ================================================= moments: THE TEETH */
S("moments: teeth (large mean, small variance)");
{
    /* The naive one-pass forms, spelled out here so the failure is demonstrated
       on the same bytes the kernel read rather than argued about. */
    const naiveVarPop = (a) => {
        let s = 0, q = 0;
        for (const v of a) { s += v; q += v * v; }
        return q / a.length - (s / a.length) * (s / a.length);
    };
    const naiveSkew = (a) => {
        const n = a.length;
        let s = 0, s2 = 0, s3 = 0;
        for (const v of a) { s += v; s2 += v * v; s3 += v * v * v; }
        const mu = s / n, m2 = s2 / n - mu * mu;
        const m3 = s3 / n - 3 * mu * s2 / n + 2 * mu * mu * mu;
        return m3 / (m2 * Math.sqrt(m2));
    };
    const naiveKurt = (a) => {
        const n = a.length;
        let s = 0, s2 = 0, s3 = 0, s4 = 0;
        for (const v of a) { s += v; s2 += v * v; s3 += v * v * v; s4 += v * v * v * v; }
        const mu = s / n, m2 = s2 / n - mu * mu;
        const m4 = s4 / n - 4 * mu * s3 / n + 6 * mu * mu * s2 / n - 3 * mu * mu * mu * mu;
        return m4 / (m2 * m2) - 3;
    };
    /* "wrong" means wrong, not merely different: NaN counts, and so does a
       plausible finite number, which is the more dangerous of the two. */
    const failsOn = (got, want, what) =>
        ok(Number.isNaN(got) || Math.abs(got - want) > 1e-6 * Math.max(1, Math.abs(want)),
           "NAIVE " + what + " FAILS on this data -- if it passes, the test has "
           + "no teeth and two-pass has no reason to exist",
           "naive " + got + " vs true " + want);

    /* 1000 rows of 1e9 + k. sum((k-499.5)^2) = n(n^2-1)/12 = 83333250, and
       every partial sum is an exact binary64 integer or quarter, so the
       two-pass answer is EXACT and `eq` is the right assertion. */
    const N = 1000;
    const x = new Float64Array(N);
    for (let i = 0; i < N; i++) x[i] = 1e9 + i;
    const dfx = new DataFrame({ x });
    eq(dfx.VARIANCE_POP("x"), 83333.25, "TEETH VARIANCE_POP(1e9+k) is exact");
    failsOn(naiveVarPop(x), 83333.25, "VARIANCE_POP");
    near(dfx.VARIANCE("x"), 83333250 / 999, "TEETH variance(1e9+k)", 1e-15);

    /* Skew and kurtosis are translation-invariant, so shifting a known shape by
       1e9 must not move the answer -- and moves it enormously for the naive
       form, because third and fourth powers put more digits above the result.
       [0,0,0,1] has skew 2/sqrt(3) and excess kurtosis -2/3, both exact. */
    const s4 = new Float64Array([1e9, 1e9, 1e9, 1e9 + 1]);
    const df4 = new DataFrame({ s: s4 });
    near(df4.SKEW("s"), 2 / Math.sqrt(3), "TEETH skew(1e9 + [0,0,0,1])", 1e-14);
    failsOn(naiveSkew(s4), 2 / Math.sqrt(3), "SKEW");
    near(df4.KURTOSIS("s"), -2 / 3, "TEETH kurtosis(1e9 + [0,0,0,1])", 1e-14);
    failsOn(naiveKurt(s4), -2 / 3, "KURTOSIS");

    /* The same shape at n = 1000, where the naive form returns a WRONG NUMBER
       instead of a NaN. Measured: skew -4.8e7 for a true 1.1547, kurtosis
       8.4e14 for a true -0.667. Nothing downstream can see either. */
    const sn = new Float64Array(N);
    for (let i = 0; i < N; i++) sn[i] = 1e9 + (i % 4 === 3 ? 1 : 0);
    const dfn = new DataFrame({ s: sn });
    near(dfn.SKEW("s"), 2 / Math.sqrt(3), "TEETH skew at n=1000", 1e-13);
    failsOn(naiveSkew(sn), 2 / Math.sqrt(3), "skew at n=1000");
    ok(Number.isFinite(naiveSkew(sn)),
       "the naive skew here fails as a plausible NUMBER, not as a NaN",
       "naive " + naiveSkew(sn));
    near(dfn.KURTOSIS("s"), -2 / 3, "TEETH kurtosis at n=1000", 1e-13);
    failsOn(naiveKurt(sn), -2 / 3, "kurtosis at n=1000");

    /* the two-column family, same conditioning: y = 3x + 7 */
    const y = new Float64Array(N);
    for (let i = 0; i < N; i++) y[i] = 3 * x[i] + 7;
    const dfxy = new DataFrame({ x, y });
    eq(dfxy.COV_POP("x", "y"), 3 * 83333.25, "TEETH COV_POP on 1e9-shifted data");
    const naiveCov = (() => {
        let sx = 0, sy = 0, sxy = 0;
        for (let i = 0; i < N; i++) { sx += x[i]; sy += y[i]; sxy += x[i] * y[i]; }
        return sxy / N - (sx / N) * (sy / N);
    })();
    failsOn(naiveCov, 3 * 83333.25, "COV_POP");
    near(dfxy.REGR_SLOPE("y", "x"), 3, "TEETH REGR_SLOPE on 1e9-shifted data", 1e-12);
}

/* ==================================== moments x dtype x tail length sweep */
S("moments: dtype x tail sweep");
{
    let cells = 0;
    for (const [tag, T] of NUMERIC) {
        for (const n of N_TAILS) {
            const a = build(T, GEN[tag], n);
            const b = build(T, (i) => GEN[tag](i * 3 + 1), n);
            const m = maskFor(n);
            const df = new DataFrame({ a, b });
            const at = tag + "[" + n + "]";
            cells++;

            /* The reference is the DEFINITION -- two plain passes -- and it is
               deliberately spelled with the same association as the kernel so
               the only expected difference is FMA contraction, which is why
               these are `near` and not `eq`: a compiler is free to fuse
               `m2 += d*d` into an fma() where the ISA has one, and the result
               is then one rounding rather than two. */
            const M = (sel) => {
                let k = 0, s = 0, sb = 0;
                for (let i = 0; i < n; i++) if (sel(i)) { k++; s += a[i]; sb += b[i]; }
                if (k === 0) return { k: 0 };
                const mx = s / k, my = sb / k;
                let m2 = 0, m3 = 0, m4 = 0, m2y = 0, cxy = 0;
                for (let i = 0; i < n; i++) if (sel(i)) {
                    const d = a[i] - mx, e = b[i] - my;
                    m2 += d * d; m3 += d * d * d; m4 += d * d * d * d;
                    m2y += e * e; cxy += d * e;
                }
                return { k, mx, my, m2, m3, m4, m2y, cxy };
            };
            const expect = (r, which) => {
                if (!r.k) return NaN;
                const v2 = r.m2 / r.k;
                switch (which) {
                case "varPop":  return v2;
                case "SKEW":    return r.m2 > 0 ? (r.m3 / r.k) / (v2 * Math.sqrt(v2)) : NaN;
                case "kurt":    return r.m2 > 0 ? (r.m4 / r.k) / (v2 * v2) - 3 : NaN;
                case "COV_POP":  return r.cxy / r.k;
                case "COV_SAMP": return r.k >= 2 ? r.cxy / (r.k - 1) : NaN;
                case "CORR":    return (r.m2 > 0 && r.m2y > 0)
                    ? Math.max(-1, Math.min(1, r.cxy / (Math.sqrt(r.m2) * Math.sqrt(r.m2y))))
                    : NaN;
                default:        return NaN;
                }
            };
            const all = M(() => true), sel = M((i) => m[i]);
            const TOL = 1e-9;

            near(df.VARIANCE_POP("a"), expect(all, "varPop"), "VARIANCE_POP " + at, TOL);
            near(df.VARIANCE_POP("a", m), expect(sel, "varPop"), "VARIANCE_POP masked " + at, TOL);
            const sp = expect(all, "varPop");
            near(df.STDDEV_POP("a"), Number.isNaN(sp) ? NaN : Math.sqrt(sp),
                 "STDDEV_POP " + at, TOL);
            near(df.SKEW("a"), expect(all, "SKEW"), "skew " + at, TOL);
            near(df.SKEW("a", m), expect(sel, "SKEW"), "skew masked " + at, TOL);
            near(df.KURTOSIS("a"), expect(all, "kurt"), "kurtosis " + at, TOL);
            near(df.KURTOSIS("a", m), expect(sel, "kurt"), "kurtosis masked " + at, TOL);

            near(df.COV_POP("a", "b"), expect(all, "COV_POP"), "COV_POP " + at, TOL);
            near(df.COV_POP("a", "b", m), expect(sel, "COV_POP"), "COV_POP masked " + at, TOL);
            near(df.COV_SAMP("a", "b"), expect(all, "COV_SAMP"), "COV_SAMP " + at, TOL);
            near(df.COV_SAMP("a", "b", m), expect(sel, "COV_SAMP"), "COV_SAMP masked " + at, TOL);
            near(df.CORR("a", "b"), expect(all, "CORR"), "corr " + at, TOL);
            near(df.CORR("a", "b", m), expect(sel, "CORR"), "corr masked " + at, TOL);

            /* regr* take (Y, X): x is the SECOND argument throughout. */
            const slope = all.k && all.m2 > 0 ? all.cxy / all.m2 : NaN;
            near(df.REGR_SLOPE("b", "a"), slope, "REGR_SLOPE " + at, TOL);
            near(df.REGR_INTERCEPT("b", "a"),
                 Number.isNaN(slope) ? NaN : all.my - slope * all.mx,
                 "REGR_INTERCEPT " + at, TOL);
            const rr = expect(all, "CORR");
            near(df.REGR_R2("b", "a"), Number.isNaN(rr) ? NaN : Math.min(1, rr * rr),
                 "REGR_R2 " + at, TOL);
            near(df.REGR_AVG_X("b", "a"), all.k ? all.mx : NaN, "REGR_AVG_X " + at, TOL);
            near(df.REGR_AVG_Y("b", "a"), all.k ? all.my : NaN, "REGR_AVG_Y " + at, TOL);

            /* SELF-covariance identities. COV_SAMP(x,x) is the sample variance
               the module already ships, computed by a DIFFERENT kernel with a
               different accumulator count -- so this crosses the two. */
            near(df.COV_POP("a", "a"), df.VARIANCE_POP("a"), "COV_POP(a,a) == VARIANCE_POP " + at, TOL);
            near(df.COV_SAMP("a", "a"), df.VARIANCE("a"), "COV_SAMP(a,a) == variance " + at, TOL);
            near(df.COV_SAMP("a", "a", m), df.VARIANCE("a", m),
                 "COV_SAMP(a,a) masked == variance masked " + at, TOL);
            /* and the /n vs /(n-1) relation, everywhere it is defined */
            const kk = all.k;
            if (kk >= 2)
                near(df.VARIANCE_POP("a") * kk / (kk - 1), df.VARIANCE("a"),
                     "VARIANCE_POP -> variance " + at, TOL);

            /* corr is symmetric; REGR_SLOPE is not */
            near(df.CORR("a", "b"), df.CORR("b", "a"), "corr is symmetric " + at, TOL);

            /* describe(): every field IS the same-named method */
            const D = df.DESCRIBE("a"), Dm = df.DESCRIBE("a", m);
            eq(D.count, df.COUNT("a"), "describe.count " + at);
            eq(Dm.count, df.COUNT("a", m), "describe.count masked " + at);
            near(D.sum, df.SUM("a"), "describe.sum " + at, TOL);
            near(D.mean, df.MEAN("a"), "describe.mean " + at, TOL);
            eq(D.min, df.MIN("a"), "describe.min " + at);
            eq(D.max, df.MAX("a"), "describe.max " + at);
            eq(Dm.min, df.MIN("a", m), "describe.min masked " + at);
            eq(Dm.max, df.MAX("a", m), "describe.max masked " + at);
            near(D.variance, df.VARIANCE("a"), "describe.variance " + at, TOL);
            near(D.stddev, df.STDDEV("a"), "describe.stddev " + at, TOL);
            near(D.skew, df.SKEW("a"), "describe.skew " + at, TOL);
            near(D.kurtosis, df.KURTOSIS("a"), "describe.kurtosis " + at, TOL);
            near(Dm.variance, df.VARIANCE("a", m), "describe.variance masked " + at, TOL);
        }
    }
    ok(cells === NUMERIC.length * N_TAILS.length,
       "moment sweep covered every dtype x length", cells + " cells");
    mark("VARIANCE_POP", "STDDEV_POP", "SKEW", "KURTOSIS", "COV_POP", "COV_SAMP",
         "CORR", "REGR_SLOPE", "REGR_INTERCEPT", "REGR_R2", "REGR_AVG_X", "REGR_AVG_Y",
         "DESCRIBE");
}

/* ============================== moments: every contract decision, pinned */
S("moments: contract decisions");
{
    /* ---- POPULATION vs SAMPLE at n == 1. They disagree, deliberately: the
       population variance of one observation is 0 and the sample variance is
       undefined. A test that demanded they agree here would force one of them
       to be wrong. */
    const d1 = new DataFrame({ a: new Float64Array([5]) });
    eq(d1.VARIANCE_POP("a"), 0, "VARIANCE_POP of ONE row is 0, not NaN");
    eq(d1.VARIANCE("a"), NaN, "variance of ONE row stays NaN (n-1 is 0)");
    eq(d1.STDDEV_POP("a"), 0, "STDDEV_POP of ONE row is 0");
    eq(d1.COV_POP("a", "a"), 0, "COV_POP of ONE row is 0");
    eq(d1.COV_SAMP("a", "a"), NaN, "COV_SAMP of ONE row is NaN");

    /* ---- ZERO rows: NaN for everything except the identities sum/count. */
    const d0 = new DataFrame({ a: new Float64Array(0) });
    eq(d0.VARIANCE_POP("a"), NaN, "VARIANCE_POP of ZERO rows is NaN");
    eq(d0.STDDEV_POP("a"), NaN, "STDDEV_POP of ZERO rows is NaN");
    eq(d0.SKEW("a"), NaN, "skew of ZERO rows is NaN");
    eq(d0.KURTOSIS("a"), NaN, "kurtosis of ZERO rows is NaN");
    eq(d0.COV_POP("a", "a"), NaN, "COV_POP of ZERO rows is NaN");
    eq(d0.CORR("a", "a"), NaN, "corr of ZERO rows is NaN");
    eq(d0.REGR_AVG_X("a", "a"), NaN, "REGR_AVG_X of ZERO rows is NaN");
    eq(d0.MEAN_WEIGHTED("a", "a"), NaN, "MEAN_WEIGHTED of ZERO rows is NaN");

    /* ---- skew/kurtosis are gated on SPREAD, not on a row count. n == 2 has a
       defined skewness -- exactly 0, by symmetry -- and an n >= 3 gate would
       return NaN for it. This line fails if such a gate is ever added. */
    const d2 = new DataFrame({ a: new Float64Array([1, 7]) });
    eq(d2.SKEW("a"), 0, "skew of TWO rows is 0, not NaN (no n>=3 gate)");
    eq(d2.KURTOSIS("a"), -2, "kurtosis of TWO rows is -2, not NaN (no n>=4 gate)");
    /* the same two values shifted: translation invariance, again */
    const d2b = new DataFrame({ a: new Float64Array([1e9 + 1, 1e9 + 7]) });
    eq(d2b.SKEW("a"), 0, "skew is translation-invariant at n=2");

    /* ---- EXCESS kurtosis. [-1,-1,1,1]: m2 = 1, m4 = 1, so excess is -2 and
       raw is 1. Nothing else distinguishes the two conventions. */
    const dk = new DataFrame({ a: new Float64Array([-1, -1, 1, 1]) });
    eq(dk.KURTOSIS("a"), -2, "kurtosis is EXCESS: -2 here, raw would be 1");

    /* ---- ZERO VARIANCE: NaN, and specifically NOT 0 and NOT a throw. 0 would
       assert "uncorrelated" where the answer is undefined, and a throw would
       make an ordinary filtered subset explode. */
    const dc = new DataFrame({
        k: new Float64Array([7, 7, 7, 7]),
        v: new Float64Array([1, 2, 3, 4]),
    });
    eq(dc.CORR("k", "v"), NaN, "corr with a constant column is NaN");
    eq(dc.CORR("v", "k"), NaN, "corr is NaN whichever side is constant");
    ok(dc.CORR("k", "v") !== 0, "corr with a constant column is NOT 0");
    eq(dc.REGR_SLOPE("v", "k"), NaN, "REGR_SLOPE with a constant X is NaN");
    eq(dc.REGR_INTERCEPT("v", "k"), NaN, "REGR_INTERCEPT with a constant X is NaN");
    eq(dc.REGR_R2("v", "k"), NaN, "REGR_R2 with a constant X is NaN");
    /* a constant Y over a varying X: SQL's REGR_R2 answers 1 here. This module
       answers NaN, so that REGR_R2 === corr^2 stays an identity. */
    eq(dc.REGR_R2("k", "v"), NaN, "REGR_R2 with a constant Y is NaN, not SQL's 1");
    eq(dc.SKEW("k"), NaN, "skew of a constant column is NaN");
    eq(dc.KURTOSIS("k"), NaN, "kurtosis of a constant column is NaN");
    eq(dc.VARIANCE_POP("k"), 0, "...but VARIANCE_POP of a constant column is 0");
    /* REGR_AVG_X/Y do not need spread at all */
    eq(dc.REGR_AVG_X("v", "k"), 7, "REGR_AVG_X works on a constant X");
    eq(dc.REGR_AVG_Y("k", "v"), 7, "REGR_AVG_Y works on a constant Y");

    /* ---- corr is CLAMPED to [-1, 1]. Rounding can push a perfect line past 1,
       and Math.acos(1.0000000000000002) is NaN. */
    const n = 1000, px = new Float64Array(n), py = new Float64Array(n);
    for (let i = 0; i < n; i++) { px[i] = 1e9 + i; py[i] = 3 * px[i] + 7; }
    const dp = new DataFrame({ x: px, y: py });
    eq(dp.CORR("x", "y"), 1, "corr of a perfect positive line is EXACTLY 1");
    ok(!Number.isNaN(Math.acos(dp.CORR("x", "y"))),
       "...so acos(corr) is not NaN");
    const pz = new Float64Array(n);
    for (let i = 0; i < n; i++) pz[i] = -2 * px[i] + 5;
    eq(new DataFrame({ x: px, z: pz }).CORR("x", "z"), -1,
       "corr of a perfect negative line is EXACTLY -1");
    eq(dp.REGR_R2("y", "x"), 1, "REGR_R2 of a perfect line is EXACTLY 1");

    /* ---- NaN PROPAGATES through the sums and is IGNORED by min/max, exactly
       as in the reduction family. */
    const dn = new DataFrame({
        a: new Float64Array([1, NaN, 3]), b: new Float64Array([1, 2, 3]),
    });
    eq(dn.VARIANCE_POP("a"), NaN, "one NaN makes VARIANCE_POP NaN");
    eq(dn.SKEW("a"), NaN, "one NaN makes skew NaN");
    eq(dn.KURTOSIS("a"), NaN, "one NaN makes kurtosis NaN");
    eq(dn.CORR("a", "b"), NaN, "a NaN in either column makes corr NaN");
    eq(dn.REGR_SLOPE("b", "a"), NaN, "a NaN in X makes REGR_SLOPE NaN");
    /* masked OUT, it is gone -- exclusion, not zeroing */
    const km = new Uint8Array([1, 0, 1]);
    eq(dn.VARIANCE_POP("a", km), 1, "a masked-out NaN is EXCLUDED, not zeroed");
    /* a tolerance, not eq: corr divides two sums of products, so a perfectly
       correlated pair lands a ulp below 1 rather than on it. Pinning the exact
       double would be pinning the arithmetic, not the contract. */
    near(dn.CORR("a", "b", km), 1, "a masked-out NaN does not poison corr", 1e-12);
    /* an Infinity behaves as the existing variance does: mean is Infinity, the
       centred deviation is Inf-Inf, and the moment is NaN */
    const di = new DataFrame({ a: new Float64Array([1, Infinity, 3]) });
    eq(di.VARIANCE_POP("a"), NaN, "an Infinity makes VARIANCE_POP NaN (as variance does)");
    eq(di.VARIANCE("a"), NaN, "...and the existing variance agrees");

    mark("VARIANCE_POP", "STDDEV_POP", "SKEW", "KURTOSIS", "COV_POP", "COV_SAMP",
         "CORR", "REGR_SLOPE", "REGR_INTERCEPT", "REGR_R2", "REGR_AVG_X", "REGR_AVG_Y",
         "MEAN_WEIGHTED");
}

/* ============ regression: the SQL argument order, which a flip breaks silently */
S("regression: argument order");
{
    /* y = 3x + 7 exactly, on small integers so every quantity is exact. The
       regr* family follows SQL:2003 REGR_*: the DEPENDENT variable is FIRST.
       Every assertion here has a DIFFERENT value under the other order, which
       is the point -- a flipped implementation cannot pass this block. */
    const N = 64;
    const x = new Float64Array(N), y = new Float64Array(N);
    for (let i = 0; i < N; i++) { x[i] = i; y[i] = 3 * i + 7; }
    const df = new DataFrame({ x, y });

    near(df.REGR_SLOPE("y", "x"), 3, "REGR_SLOPE(Y, X) is 3", 1e-13);
    near(df.REGR_SLOPE("x", "y"), 1 / 3, "REGR_SLOPE(X, Y) is 1/3 -- the orders DIFFER", 1e-13);
    near(df.REGR_INTERCEPT("y", "x"), 7, "REGR_INTERCEPT(Y, X) is 7", 1e-12);
    near(df.REGR_INTERCEPT("x", "y"), -7 / 3, "REGR_INTERCEPT(X, Y) is -7/3", 1e-12);

    /* REGR_AVG_X is the mean of the SECOND argument, REGR_AVG_Y of the FIRST.
       These two lines are the whole reason those two names exist. */
    eq(df.REGR_AVG_X("y", "x"), df.MEAN("x"), "REGR_AVG_X(Y, X) == mean of X (the 2nd arg)");
    eq(df.REGR_AVG_Y("y", "x"), df.MEAN("y"), "REGR_AVG_Y(Y, X) == mean of Y (the 1st arg)");
    ok(df.REGR_AVG_X("y", "x") !== df.REGR_AVG_Y("y", "x"),
       "REGR_AVG_X and REGR_AVG_Y are not the same number on this data");

    /* r2 is symmetric even though slope is not -- and it is exactly corr^2 */
    near(df.REGR_R2("y", "x"), df.REGR_R2("x", "y"), "REGR_R2 is symmetric", 1e-15);
    const c = df.CORR("x", "y");
    near(df.REGR_R2("y", "x"), c * c, "REGR_R2 IS corr squared", 1e-15);

    /* slope * (sd_x/sd_y) == corr: the textbook identity, over a DIFFERENT
       route through the same moments. */
    near(df.REGR_SLOPE("y", "x") * df.STDDEV_POP("x") / df.STDDEV_POP("y"), c,
         "slope * sd(x)/sd(y) == corr", 1e-13);

    /* covariance ties back to variance the same way */
    near(df.REGR_SLOPE("y", "x"), df.COV_POP("x", "y") / df.VARIANCE_POP("x"),
         "slope == cov(x,y)/var(x)", 1e-13);
    near(df.COV_SAMP("x", "y"), df.COV_POP("x", "y") * N / (N - 1),
         "COV_SAMP == COV_POP * n/(n-1)", 1e-13);

    /* the fit reproduces the data: intercept + slope*x == y at both ends */
    const b0 = df.REGR_INTERCEPT("y", "x"), b1 = df.REGR_SLOPE("y", "x");
    near(b0 + b1 * x[0], y[0], "the fitted line passes through the first point", 1e-12);
    near(b0 + b1 * x[N - 1], y[N - 1], "...and the last", 1e-12);

    mark("REGR_SLOPE", "REGR_INTERCEPT", "REGR_R2", "REGR_AVG_X", "REGR_AVG_Y",
         "CORR", "COV_POP", "COV_SAMP", "VARIANCE_POP", "STDDEV_POP");
}

/* ================================================== MEAN_WEIGHTED */
S("MEAN_WEIGHTED");
{
    const df = new DataFrame({
        v: new Float64Array([1, 2, 3, 4]),
        w: new Float64Array([1, 1, 1, 1]),
        u: new Float64Array([4, 3, 2, 1]),
        t: new Float64Array([10, 20, 30, 40]),
    });
    eq(df.MEAN_WEIGHTED("v", "w"), 2.5, "unit weights give the plain mean");
    eq(df.MEAN_WEIGHTED("v", "w"), df.MEAN("v"), "...literally mean()");
    /* (1*4 + 2*3 + 3*2 + 4*1) / 10 = 20/10 = 2 */
    eq(df.MEAN_WEIGHTED("v", "u"), 2, "real weights");
    /* The arguments are (value, weight) IN THAT ORDER, and these two numbers
       differ by a factor of ten, so a swapped implementation cannot pass both:
       (1*10+2*20+3*30+4*40)/100 = 3 against (10*1+20*2+30*3+40*4)/10 = 30. */
    eq(df.MEAN_WEIGHTED("v", "t"), 3, "MEAN_WEIGHTED(value, weight) == 3 here");
    eq(df.MEAN_WEIGHTED("t", "v"), 30, "...and the SWAPPED call is 30, not 3");

    /* A ZERO weight EXCLUDES the row; it does not scale it by zero. The two
       differ exactly here: 0 * Infinity is NaN, so a multiply would let a row
       the caller weighted OUT poison the whole answer. */
    const dz = new DataFrame({
        v: new Float64Array([1, Infinity]), w: new Float64Array([1, 0]),
    });
    eq(dz.MEAN_WEIGHTED("v", "w"), 1,
       "a ZERO weight beside an Infinity EXCLUDES the row (a multiply gives NaN)");
    const dz2 = new DataFrame({
        v: new Float64Array([1, NaN]), w: new Float64Array([1, 0]),
    });
    eq(dz2.MEAN_WEIGHTED("v", "w"), 1, "a ZERO weight beside a NaN also excludes");
    /* but a NaN WEIGHT is not zero, and propagates */
    const dz3 = new DataFrame({
        v: new Float64Array([1, 2]), w: new Float64Array([1, NaN]),
    });
    eq(dz3.MEAN_WEIGHTED("v", "w"), NaN, "a NaN WEIGHT propagates");

    /* NEGATIVE weights are ACCEPTED, not refused: contrast weights and signed
       exposures are real. This line fails if a rejection is ever added. */
    const dneg = new DataFrame({
        v: new Float64Array([1, 3]), w: new Float64Array([-1, 3]),
    });
    eq(dneg.MEAN_WEIGHTED("v", "w"), (-1 * 1 + 3 * 3) / 2, "NEGATIVE weights are accepted");

    /* but a total weight of ZERO is NaN, not +/-Infinity: the mean is
       undefined there and an infinity would read as a limit that is not there */
    const dsum0 = new DataFrame({
        v: new Float64Array([1, 3]), w: new Float64Array([2, -2]),
    });
    eq(dsum0.MEAN_WEIGHTED("v", "w"), NaN, "S(w) == 0 gives NaN, not Infinity");
    ok(!Number.isFinite(dsum0.MEAN_WEIGHTED("v", "w")) &&
       Number.isNaN(dsum0.MEAN_WEIGHTED("v", "w")),
       "...specifically NaN and not an Infinity");
    /* all weights zero: also NaN, by the same rule */
    const dallz = new DataFrame({
        v: new Float64Array([1, 3]), w: new Float64Array([0, 0]),
    });
    eq(dallz.MEAN_WEIGHTED("v", "w"), NaN, "all weights zero gives NaN");

    /* the mask selects rows out of BOTH columns */
    const m = new Uint8Array([1, 1, 0, 0]);
    eq(df.MEAN_WEIGHTED("v", "u", m), (1 * 4 + 2 * 3) / 7, "masked MEAN_WEIGHTED");
    eq(df.MEAN_WEIGHTED("v", "w", m), df.MEAN("v", m), "masked, unit weights == masked mean");

    /* mixed dtypes: the weight column need not match the value column */
    const dmix = new DataFrame({
        v: new Float64Array([1, 2, 3]), w: new Int32Array([1, 2, 3]),
    });
    eq(dmix.MEAN_WEIGHTED("v", "w"), (1 + 4 + 9) / 6, "an Int32 weight column");

    mark("MEAN_WEIGHTED");
}

/* ======================================================== describe */
S("DESCRIBE");
{
    const df = new DataFrame({ a: new Float64Array([1, 2, 3, 4]) });
    const d = df.DESCRIBE("a");

    /* the SHAPE is pinned: nine plain named properties, no more, no fewer.
       Adding a field is a deliberate act, and this line is where it is
       acknowledged. */
    const keys = Object.keys(d).sort().join(",");
    eq(keys, "count,kurtosis,max,mean,min,skew,stddev,sum,variance",
       "describe returns exactly these nine fields");
    ok(Object.getPrototypeOf(d) === Object.prototype,
       "describe returns a plain object");

    /* EVERY field is the same-named method on the same data. Nothing here is
       recomputed: that identity IS the contract of describe. */
    eq(d.count, df.COUNT("a"), "describe.count == count()");
    eq(d.sum, df.SUM("a"), "describe.sum == sum()");
    eq(d.mean, df.MEAN("a"), "describe.mean == mean()");
    eq(d.min, df.MIN("a"), "describe.min == min()");
    eq(d.max, df.MAX("a"), "describe.max == max()");
    eq(d.variance, df.VARIANCE("a"), "describe.variance == variance() -- SAMPLE");
    eq(d.stddev, df.STDDEV("a"), "describe.stddev == stddev()");
    eq(d.skew, df.SKEW("a"), "describe.skew == skew()");
    eq(d.kurtosis, df.KURTOSIS("a"), "describe.kurtosis == kurtosis()");
    /* and specifically NOT the population forms, which is the pair most likely
       to be confused */
    ok(d.variance !== df.VARIANCE_POP("a"),
       "describe.variance is the SAMPLE one, not VARIANCE_POP");

    /* masked */
    const m = new Uint8Array([1, 1, 0, 1]);
    const dm = df.DESCRIBE("a", m);
    eq(dm.count, df.COUNT("a", m), "masked describe.count");
    eq(dm.mean, df.MEAN("a", m), "masked describe.mean");
    eq(dm.min, df.MIN("a", m), "masked describe.min");
    eq(dm.max, df.MAX("a", m), "masked describe.max");
    eq(dm.variance, df.VARIANCE("a", m), "masked describe.variance");
    ok(dm.count === 3, "the mask actually excluded a row", "count " + dm.count);

    /* EMPTY: every field takes its own method's empty answer, and they are not
       all the same answer. sum is 0, mean is NaN, min/max are undefined. */
    const d0 = new DataFrame({ a: new Float64Array(0) }).DESCRIBE("a");
    eq(d0.count, 0, "empty describe.count is 0");
    eq(d0.sum, 0, "empty describe.sum is 0 (sum's identity)");
    eq(d0.mean, NaN, "empty describe.mean is NaN");
    same(d0.min, undefined, "empty describe.min is undefined, not the seed");
    same(d0.max, undefined, "empty describe.max is undefined");
    eq(d0.variance, NaN, "empty describe.variance is NaN");
    eq(d0.stddev, NaN, "empty describe.stddev is NaN");
    eq(d0.skew, NaN, "empty describe.skew is NaN");
    eq(d0.kurtosis, NaN, "empty describe.kurtosis is NaN");
    /* a mask that selects nothing must behave exactly like an empty column */
    const dz = df.DESCRIBE("a", new Uint8Array(4));
    eq(dz.count, 0, "a mask selecting nothing gives count 0");
    same(dz.min, undefined, "...and min undefined");
    eq(dz.sum, 0, "...and sum 0");

    /* NaN: min/max IGNORE it, mean/sum/variance PROPAGATE it. Those two
       families disagree deliberately and describe must reproduce BOTH. */
    const dn = new DataFrame({ a: new Float64Array([1, NaN, 3]) });
    const n = dn.DESCRIBE("a");
    eq(n.min, 1, "describe.min ignores NaN");
    eq(n.max, 3, "describe.max ignores NaN");
    eq(n.min, dn.MIN("a"), "...exactly as min() does");
    eq(n.mean, NaN, "describe.mean propagates NaN");
    eq(n.count, 3, "describe.count counts the NaN row");

    /* an ALL-NaN column: min()/max() return the seeds today (+Inf/-Inf) and
       describe MIRRORS that rather than quietly improving it. If min() is ever
       changed to return undefined here, this line fails and describe gets
       changed with it. */
    const da = new DataFrame({ a: new Float64Array([NaN, NaN]) });
    const dd = da.DESCRIBE("a");
    eq(dd.min, da.MIN("a"), "all-NaN describe.min tracks min()");
    eq(dd.max, da.MAX("a"), "all-NaN describe.max tracks max()");
    eq(dd.min, Infinity, "...which is +Infinity today");

    mark("DESCRIBE");
}

/* ====================================== moments: refusals and lifetime */
S("moments: refusals");
{
    const df = new DataFrame({
        v: new Float64Array([1, 2, 3]), label: ["x", "y", "x"],
    });
    /* a string column is dictionary-encoded; a moment of the CODES would be a
       plausible number and is refused instead */
    throwsLike(() => df.VARIANCE_POP("label"), "string column", "VARIANCE_POP on a string column");
    throwsLike(() => df.STDDEV_POP("label"), "string column", "STDDEV_POP on a string column");
    throwsLike(() => df.SKEW("label"), "string column", "skew on a string column");
    throwsLike(() => df.KURTOSIS("label"), "string column", "kurtosis on a string column");
    throwsLike(() => df.DESCRIBE("label"), "string column", "describe on a string column");
    /* a two-column method must name WHICH column is the string one */
    throwsLike(() => df.CORR("v", "label"), "'label'", "corr names the string column");
    throwsLike(() => df.CORR("label", "v"), "'label'", "corr names it on either side");
    throwsLike(() => df.CORR("v", "label"), "CORR", "...and names the method");
    throwsLike(() => df.COV_POP("v", "label"), "COV_POP", "COV_POP names the method");
    throwsLike(() => df.REGR_SLOPE("v", "label"), "REGR_SLOPE", "REGR_SLOPE names the method");
    throwsLike(() => df.MEAN_WEIGHTED("v", "label"), "MEAN_WEIGHTED",
               "MEAN_WEIGHTED refuses a string WEIGHT column");
    throwsLike(() => df.MEAN_WEIGHTED("label", "v"), "MEAN_WEIGHTED",
               "MEAN_WEIGHTED refuses a string VALUE column");

    /* unknown columns, on both argument positions */
    throwsLike(() => df.VARIANCE_POP("nope"), "no such column", "VARIANCE_POP unknown column");
    throwsLike(() => df.CORR("v", "nope"), "no such column", "corr unknown SECOND column");
    throwsLike(() => df.CORR("nope", "v"), "no such column", "corr unknown FIRST column");
    throwsLike(() => df.REGR_SLOPE("v", "nope"), "no such column", "REGR_SLOPE unknown column");
    throwsLike(() => df.DESCRIBE("nope"), "no such column", "describe unknown column");
    throws(() => df.VARIANCE_POP(), "VARIANCE_POP with no argument");
    throws(() => df.CORR("v"), "corr with one argument");

    /* an already-detached buffer must throw and name the column */
    const buf = new ArrayBuffer(8 * 8);
    const v = new Float64Array(buf);
    const dd = new DataFrame({ v, w: new Float64Array(8) });
    buf.transfer();
    throwsLike(() => dd.VARIANCE_POP("v"), "detached", "VARIANCE_POP on a detached buffer");
    throwsLike(() => dd.SKEW("v"), "detached", "skew on a detached buffer");
    throwsLike(() => dd.CORR("v", "w"), "detached", "corr on a detached buffer");
    throwsLike(() => dd.CORR("w", "v"), "detached", "corr, detached column SECOND");
    throwsLike(() => dd.REGR_SLOPE("w", "v"), "detached", "REGR_SLOPE on a detached buffer");
    throwsLike(() => dd.MEAN_WEIGHTED("w", "v"), "detached", "MEAN_WEIGHTED on a detached buffer");
    throwsLike(() => dd.DESCRIBE("v"), "detached", "describe on a detached buffer");

    /* a bad mask is refused the same way it is everywhere else */
    const dm = new DataFrame({ a: new Float64Array(8) });
    throwsLike(() => dm.VARIANCE_POP("a", new Uint8Array(3)), "at least",
               "a short mask is refused");
    throws(() => dm.CORR("a", "a", new Float64Array(8)), "a non-Uint8Array mask is refused");

    mark("VARIANCE_POP", "STDDEV_POP", "SKEW", "KURTOSIS", "COV_POP", "CORR",
         "REGR_SLOPE", "MEAN_WEIGHTED", "DESCRIBE");
}

/* Lifetime: the coerce-EVERYTHING-then-bind rule, attacked per method. Each of
   these hooks a coercion the method ACTUALLY performs -- a column name is
   reached through toString, never valueOf -- and `attack` fails the run if the
   hook never fired, because an attack that exercises nothing passes silently. */
{
    attack("detach during VARIANCE_POP's column-name coercion", (fire) => {
        const { df, buf } = hostile();
        return df.VARIANCE_POP({ toString() { fire(); buf.transfer(); return "v"; } });
    });
}
{
    attack("detach during corr's SECOND column name (first already resolved)", (fire) => {
        const { df, buf } = hostile();
        return df.CORR("v", { toString() { fire(); buf.transfer(); return "v"; } });
    });
}
{
    attack("detach during REGR_SLOPE's SECOND column name", (fire) => {
        const { df, buf } = hostile();
        return df.REGR_SLOPE("v", { toString() { fire(); buf.transfer(); return "v"; } });
    });
}
{
    attack("detach during MEAN_WEIGHTED's WEIGHT column name", (fire) => {
        const buf = new ArrayBuffer(8 * 8);
        const w = new Float64Array(buf); w.fill(1);
        const df = new DataFrame({ v: new Float64Array(8), w });
        return df.MEAN_WEIGHTED("v", { toString() { fire(); buf.transfer(); return "w"; } });
    });
}
{
    attack("detach during describe's column-name coercion", (fire) => {
        const { df, buf } = hostile();
        return df.DESCRIBE({ toString() { fire(); buf.transfer(); return "v"; } });
    });
}
{   /* the MASK buffer, detached through a column name -- this one only fails if
       the mask pointer is taken BEFORE the names are coerced */
    attack("detach the MASK buffer during corr's column-name coercion", (fire) => {
        const mbuf = new ArrayBuffer(64);
        const mask = new Uint8Array(mbuf); mask.fill(1);
        const df = new DataFrame({ v: new Float64Array(64), u: new Float64Array(64) });
        return df.CORR("v", { toString() { fire(); mbuf.transfer(); return "u"; } }, mask);
    });
}
{   /* a resizable buffer SHRUNK mid-coercion: the view stays attached, so an
       is-detached check alone misses it and the range must be re-validated */
    attack("shrink a resizable buffer during describe's coercion", (fire) => {
        const rb = new ArrayBuffer(8 * 64, { maxByteLength: 8 * 64 });
        const df = new DataFrame({ v: new Float64Array(rb) });
        return df.DESCRIBE({ toString() { fire(); try { rb.resize(8); } catch (e) {} return "v"; } });
    });
}

/* ================================================ generated: g6 */

/* ============================================ logical reductions (truthiness) */
S("logical reductions");
{
    /* Values chosen so every dtype stores them exactly and the truthy COUNT is
       not a constant fraction of n: 0 and -1 are the two that matter, -1
       because it is falsy nowhere and wraps to a large truthy value in an
       unsigned column, which the reference reads back from the array. */
    const LGEN = (i) => [0, 1, 0, -1, 2][i % 5];

    let cells = 0;
    for (const [tag, T] of NUMERIC) {
        for (const n of N_TAILS) {
            const a = build(T, LGEN, n);
            const m = maskFor(n);
            const df = new DataFrame({ a });
            const at = " " + tag + " n=" + n;
            cells++;

            /* The reference is literally Boolean(), read back from the column
               so that any storage conversion is reflected in both. */
            let nt = 0, ns = n, mt = 0, ms = 0;
            for (let i = 0; i < n; i++) {
                const t = Boolean(a[i]);
                if (t) nt++;
                if (m[i]) { ms++; if (t) mt++; }
            }
            eq(df.BOOL_AND("a"), nt === ns, "BOOL_AND" + at);
            eq(df.BOOL_OR("a"), nt > 0, "BOOL_OR" + at);
            eq(df.BOOL_XOR("a"), (nt & 1) === 1, "BOOL_XOR" + at);
            eq(df.BOOL_AND("a", m), mt === ms, "BOOL_AND masked" + at);
            eq(df.BOOL_OR("a", m), mt > 0, "BOOL_OR masked" + at);
            eq(df.BOOL_XOR("a", m), (mt & 1) === 1, "BOOL_XOR masked" + at);
        }
    }
    ok(cells === NUMERIC.length * N_TAILS.length,
       "logical sweep covered every dtype x length", cells + " cells");

    /* ---- NaN IS FALSY, and the naive predicate is not merely different, it
       is WRONG. This is the sharpest case in the family: every other value
       agrees between the two spellings. */
    const naiveAny = (a) => {
        for (let i = 0; i < a.length; i++) if (a[i] != 0) return true;  /* NaN != 0 */
        return false;
    };
    for (const T of [Float64Array, Float32Array]) {
        const nm = T.name;
        const a = new T([NaN, NaN, NaN]);
        const df = new DataFrame({ a });
        eq(df.BOOL_OR("a"), false, nm + " all-NaN BOOL_OR -> false (Boolean(NaN))");
        eq(df.BOOL_AND("a"), false, nm + " all-NaN BOOL_AND -> false");
        eq(df.BOOL_XOR("a"), false, nm + " all-NaN BOOL_XOR -> false (0 truthy is even)");
        ok(naiveAny(a) === true,
           "the naive `v != 0` predicate calls NaN TRUE -- the trap is live", nm);
        ok(naiveAny(a) !== df.BOOL_OR("a"),
           "module and naive predicate DISAGREE on an all-NaN column", nm);

        const mix = new DataFrame({ a: new T([NaN, 1]) });
        eq(mix.BOOL_AND("a"), false, nm + " [NaN,1] BOOL_AND -> false");
        eq(mix.BOOL_OR("a"), true, nm + " [NaN,1] BOOL_OR -> true");
        eq(mix.BOOL_XOR("a"), true, nm + " [NaN,1] BOOL_XOR -> true (one truthy)");
    }

    /* ---- -0 is falsy and +-Infinity is truthy, exactly as Boolean() says. */
    {
        const z = new Float64Array([-0, -0]);
        same(z[0], -0, "the column really holds -0, not +0");
        const dz = new DataFrame({ a: z });
        eq(dz.BOOL_OR("a"), false, "-0 is falsy (BOOL_OR)");
        eq(dz.BOOL_AND("a"), false, "-0 is falsy (BOOL_AND)");
        const di = new DataFrame({ a: new Float64Array([Infinity, -Infinity]) });
        eq(di.BOOL_AND("a"), true, "+-Infinity is truthy");
        eq(di.BOOL_XOR("a"), false, "two truthy -> BOOL_XOR false");
    }

    /* ---- Empty and all-masked-out give the SAME vacuous answers. This is the
       only input that exposes a wrong accumulator seed. */
    for (const [tag, T] of NUMERIC) {
        const e = new DataFrame({ a: new T(0) });
        eq(e.BOOL_AND("a"), true, "empty " + tag + " BOOL_AND -> true (vacuous)");
        eq(e.BOOL_OR("a"), false, "empty " + tag + " BOOL_OR -> false");
        eq(e.BOOL_XOR("a"), false, "empty " + tag + " BOOL_XOR -> false");

        const f = new DataFrame({ a: build(T, () => 1, 4) });
        const none = new Uint8Array(4);
        eq(f.BOOL_AND("a", none), true, "all-masked-out " + tag + " BOOL_AND -> true");
        eq(f.BOOL_OR("a", none), false, "all-masked-out " + tag + " BOOL_OR -> false");
        eq(f.BOOL_XOR("a", none), false, "all-masked-out " + tag + " BOOL_XOR -> false");
    }

    /* ---- A masked-out row is EXCLUDED, never zeroed. Each of these flips if
       an excluded row is folded in as 0 instead of skipped. */
    {
        const m = new Uint8Array([1, 1, 0]);
        const dA = new DataFrame({ a: new Int32Array([1, 1, 0]) });
        eq(dA.BOOL_AND("a"), false, "unmasked BOOL_AND sees the falsy row");
        eq(dA.BOOL_AND("a", m), true,
           "masking out the ONLY falsy row -> BOOL_AND true (exclude, not zero)");
        const dO = new DataFrame({ a: new Int32Array([0, 0, 1]) });
        eq(dO.BOOL_OR("a"), true, "unmasked BOOL_OR sees the truthy row");
        eq(dO.BOOL_OR("a", m), false, "masking out the ONLY truthy row -> BOOL_OR false");
        const dX = new DataFrame({ a: new Int32Array([1, 1, 1]) });
        eq(dX.BOOL_XOR("a"), true, "three truthy -> BOOL_XOR true");
        eq(dX.BOOL_XOR("a", m), false, "mask drops one -> two truthy -> BOOL_XOR false");
    }

    /* ---- BOOL_AND IS NOT BITWISE_AND. 1 & 2 is 0; both are truthy. A single
       column where the two families disagree is worth more than either alone. */
    {
        const d = new DataFrame({ a: new Uint8Array([1, 2]) });
        eq(d.BOOL_AND("a"), true, "BOOL_AND([1,2]) -> true (both truthy)");
        eq(d.BITWISE_AND("a"), 0, "BITWISE_AND([1,2]) -> 0 (bit fold), and that is fine");
        eq(d.BOOL_XOR("a"), false, "BOOL_XOR([1,2]) -> false (two truthy)");
        eq(d.BITWISE_XOR("a"), 3, "BITWISE_XOR([1,2]) -> 3");
    }

    /* ---- BOOL_AND IS NOT all() EITHER, but they must agree where they overlap:
       a Uint8Array column with no row mask. `all` folds the bytes of an
       arbitrary mask; BOOL_AND folds the values of a named column of any dtype.
       Neither can express the other -- all("a") is a TypeError, and BOOL_AND
       cannot be handed an array that is not a column -- so this assertion is
       what catches the two truthiness predicates drifting apart. */
    for (const n of [0, 1, 7, 33, 4096, 4097]) {
        for (const [sn, mk] of Object.entries({
            allTrue: (u) => u.fill(1),
            allFalse: (u) => u,
            firstOnly: (u) => { if (u.length) u[0] = 1; return u; },
            lastZero: (u) => { u.fill(1); if (u.length) u[u.length - 1] = 0; return u; },
            twos: (u) => u.fill(2),
        })) {
            const a = mk(new Uint8Array(n));
            const df = new DataFrame({ a });
            const at = " n=" + n + " " + sn;
            eq(df.BOOL_AND("a"), df.ALL(a), "BOOL_AND agrees with all()" + at);
            eq(df.BOOL_OR("a"), df.ANY(a), "BOOL_OR agrees with any()" + at);
        }
    }

    /* ---- Refusals. A dictionary code is not a value the caller stored, so a
       string column is named rather than answered. */
    {
        const s = new DataFrame({ a: ["x", "y"] });
        throwsLike(() => s.BOOL_AND("a"), "string column", "BOOL_AND refuses a string column");
        throwsLike(() => s.BOOL_AND("a"), "BOOL_AND", "and names itself");
        throwsLike(() => s.BOOL_OR("a"), "BOOL_OR", "BOOL_OR refuses a string column");
        throwsLike(() => s.BOOL_XOR("a"), "BOOL_XOR", "BOOL_XOR refuses a string column");
        throwsLike(() => s.BOOL_AND("nope"), "no such column", "unknown column");

        /* the mask is OPTIONAL here, unlike all()/any() which require one */
        const d = new DataFrame({ a: new Float64Array([1, 1]) });
        eq(d.BOOL_AND("a"), true, "BOOL_AND with no mask is legal (all() is not)");
        throwsLike(() => d.BOOL_AND("a", new Uint8Array(1)), "at least",
                   "a short mask is refused");
        throws(() => d.BOOL_AND(), "BOOL_AND() with no column at all");
    }
    mark("BOOL_AND", "BOOL_OR", "BOOL_XOR");
}

/* ================================= SUM_CHECKED: the exact-integer sum contract */
S("SUM_CHECKED");
{
    /* ---- Wherever the total is representable, SUM_CHECKED IS sum. It differs
       from sum only by refusing to return a rounded answer.
       Two oracles, and the second is not redundant: the JS reference is the
       definition, and sum() is an INDEPENDENT kernel (a different accumulator
       family in the same file), so a disagreement localises to one of them. */
    let cells = 0;
    for (const [tag, T] of INTS) {
        for (const n of N_TAILS) {
            const a = build(T, (i) => [0, 1, -1, 100, -100][i % 5], n);
            const m = maskFor(n);
            const df = new DataFrame({ a });
            const at = " " + tag + " n=" + n;
            cells++;
            let rs = 0, ms = 0;
            for (let i = 0; i < n; i++) { rs += a[i]; if (m[i]) ms += a[i]; }
            eq(df.SUM_CHECKED("a"), rs, "SUM_CHECKED" + at);
            eq(df.SUM_CHECKED("a", m), ms, "SUM_CHECKED masked" + at);
            eq(df.SUM_CHECKED("a"), df.SUM("a"), "SUM_CHECKED == sum" + at);
            eq(df.SUM_CHECKED("a", m), df.SUM("a", m), "SUM_CHECKED == sum masked" + at);
        }
        eq(new DataFrame({ a: new T(0) }).SUM_CHECKED("a"), 0,
           "empty " + tag + " SUM_CHECKED -> 0");
    }
    ok(cells === INTS.length * N_TAILS.length,
       "SUM_CHECKED sweep covered every integer dtype x length", cells + " cells");

    /* ---- Refusals. "Checked" means integer exactness; a float sum's error is
       rounding at every step, so answering "not overflowed" for a Float64Array
       would be the plausible wrong answer. */
    throwsLike(() => new DataFrame({ f: new Float64Array([1.5]) }).SUM_CHECKED("f"),
               "Float64Array", "SUM_CHECKED names the offending float dtype");
    throwsLike(() => new DataFrame({ f: new Float32Array([1.5]) }).SUM_CHECKED("f"),
               "use sum()", "and names the method the caller should have used");
    throwsLike(() => new DataFrame({ a: ["x"] }).SUM_CHECKED("a"), "string column",
               "SUM_CHECKED refuses a string column");
    throwsLike(() => new DataFrame({ a: new Int32Array(4) }).SUM_CHECKED("a", new Uint8Array(2)),
               "at least", "SUM_CHECKED inherits the mask length check");

    /* ---- THE BOUNDARY. Every assertion in this group fails against an
       implementation that just returns the double -- which is what sum() does,
       and it is asserted here doing exactly that, next to the refusal.
       (2^32-1) * 2^21 = 2^53 - 2^21, so one more row sets the last bits. */
    {
        const n = 2097152;
        const a = new Uint32Array(n + 1);
        a.fill(4294967295, 0, n);

        a[n] = 2097152;                       /* total = 2^53 exactly */
        let df = new DataFrame({ a });
        eq(df.SUM_CHECKED("a"), 9007199254740992, "total exactly 2^53 is returned");
        eq(df.SUM("a"), 9007199254740992, "and sum() agrees there");

        a[n] = 2097153;                       /* total = 2^53 + 1 */
        df = new DataFrame({ a });
        eq(df.SUM("a"), 9007199254740992,
           "sum() SILENTLY ROUNDS 2^53+1 down to 9007199254740992");
        const e = throws(() => df.SUM_CHECKED("a"), "SUM_CHECKED refuses 2^53+1");
        if (e) {
            ok(e instanceof RangeError, "the refusal is a RangeError", String(e));
            ok(String(e.message).indexOf("9007199254740993") >= 0,
               "and the message carries the EXACT total", e.message);
        }

        /* the check runs on the SELECTED total, not on the whole column */
        const m = new Uint8Array(n + 1).fill(1);
        m[n] = 0;
        eq(df.SUM_CHECKED("a", m), 9007199252643840,
           "masking out the last row brings the total back into exact range");

        a[n] = 2097154;                       /* total = 2^53 + 2, EXACT */
        df = new DataFrame({ a });
        eq(df.SUM_CHECKED("a"), 9007199254740994,
           "2^53+2 is past 2^53 and exactly representable, so it is RETURNED");
    }

    /* ---- The signed accumulator, on the negative side of the same cliff.
       -2^31 * 2^22 = -2^53. */
    {
        const n = 4194304;
        const a = new Int32Array(n + 1);
        a.fill(-2147483648, 0, n);

        a[n] = 0;
        eq(new DataFrame({ a }).SUM_CHECKED("a"), -9007199254740992,
           "total exactly -2^53 is returned");
        a[n] = -1;
        const df = new DataFrame({ a });
        eq(df.SUM("a"), -9007199254740992, "sum() rounds -(2^53+1) too");
        const e = throws(() => df.SUM_CHECKED("a"), "SUM_CHECKED refuses -(2^53+1)");
        if (e) ok(String(e.message).indexOf("-9007199254740993") >= 0,
                  "the message carries the exact negative total", e.message);
    }
    mark("SUM_CHECKED");
}

/* ================================================ generated: g7 */

/* ------------------------------------------------------------------ [G7-A] */

/* ================================================= grouped aggregate family */
S("grouped aggregates");
{
    const city = ["NY", "SF", "NY", "LA", "SF", "NY"];
    const amt = new Float64Array([1, 2, 3, 4, 5, 6]);
    const df = new DataFrame({ city, amt });

    eq(df.GROUP_BY_MIN("city", "amt").keys.join(","), "NY,SF,LA",
       "grouped keys are the dictionary, same as GROUP_BY_SUM");
    elemEq(df.GROUP_BY_MIN("city", "amt").values, [1, 2, 4], "GROUP_BY_MIN");
    elemEq(df.GROUP_BY_MAX("city", "amt").values, [6, 5, 4], "GROUP_BY_MAX");
    elemEq(df.GROUP_BY_MEAN("city", "amt").values, [10 / 3, 3.5, 4], "GROUP_BY_MEAN", 1e-15);
    elemEq(df.GROUP_BY_COUNT("city").values, [3, 2, 1], "GROUP_BY_COUNT");

    /* masked: the same mask GROUP_BY_SUM takes, in the same position */
    const m = df.GT("amt", 2);                       /* [0,0,1,1,1,1] */
    elemEq(df.GROUP_BY_MIN("city", "amt", m).values, [3, 5, 4], "masked GROUP_BY_MIN");
    elemEq(df.GROUP_BY_MAX("city", "amt", m).values, [6, 5, 4], "masked GROUP_BY_MAX");
    elemEq(df.GROUP_BY_MEAN("city", "amt", m).values, [4.5, 5, 4], "masked GROUP_BY_MEAN");
    elemEq(df.GROUP_BY_COUNT("city", m).values, [2, 1, 1], "masked GROUP_BY_COUNT");

    /* the value column may be any numeric type, and every integer width may be
       a key -- both inherited from GROUP_BY_SUM's key/value handling */
    for (const [tag, T] of INTS) {
        const d = new DataFrame({ k: new T([0, 1, 0]), v: new Float64Array([1, 2, 3]) });
        elemEq(d.GROUP_BY_MAX("k", "v").values, [3, 2], "GROUP_BY_MAX over a " + tag + " key");
        elemEq(d.GROUP_BY_COUNT("k").values, [2, 1], "GROUP_BY_COUNT over a " + tag + " key");
    }
    elemEq(new DataFrame({ k: new Int32Array([0, 0, 1]), v: new Int16Array([7, 9, 3]) })
              .GROUP_BY_MIN("k", "v").values, [7, 3], "an integer value column");

    /* integer keys are DENSE 0..max, and a group nothing lands in is EMPTY */
    const sp = new DataFrame({ k: new Int32Array([0, 100]), v: new Float64Array([1, 2]) });
    eq(sp.GROUP_BY_MIN("k", "v").keys.length, 101, "integer keys are dense 0..max");
    eq(sp.GROUP_BY_MIN("k", "v").values[100], 2, "the far key lands");
    eq(sp.GROUP_BY_MIN("k", "v").values[50], NaN,
       "a dense group nothing landed in is NaN, NOT +Infinity");
    eq(sp.GROUP_BY_COUNT("k").values[50], 0, "and its count is 0");

    mark("GROUP_BY_MIN", "GROUP_BY_MAX", "GROUP_BY_MEAN", "GROUP_BY_COUNT");
}

/* ======================= THE EMPTY GROUP, and why it is not the all-NaN group */
S("empty group vs all-NaN group");
{
    /* TWO DIFFERENT QUESTIONS, and the module answers them differently on
       purpose. The scalar reduction ends `count ? acc : undefined`: it refuses
       to publish min's +Infinity SEED when nothing contributed, but an all-NaN
       column DID contribute rows -- count is the row count -- so it surfaces
       the seed (pinned above as `all-NaN[n] min -> +Infinity`). A Float64Array
       cannot hold `undefined`, so the count == 0 case becomes NaN here.
       Collapsing the two either hides an empty group behind an infinity or
       reports an all-NaN group as missing. Both are plausible wrong answers. */
    const d = new DataFrame({ c: ["NY", "SF", "NY"], v: new Float64Array([1, 2, 3]) });
    const m = new Uint8Array([1, 0, 1]);             /* SF has no rows left */

    eq(d.GROUP_BY_MIN("c", "v", m).keys.join(","), "NY,SF",
       "an EMPTY group keeps its key -- keys and values stay index-aligned");
    elemEq(d.GROUP_BY_MIN("c", "v", m).values, [1, NaN],
           "an EMPTY group's min is NaN, not +Infinity");
    elemEq(d.GROUP_BY_MAX("c", "v", m).values, [3, NaN],
           "an EMPTY group's max is NaN, not -Infinity");
    elemEq(d.GROUP_BY_MEAN("c", "v", m).values, [2, NaN], "an EMPTY group's mean is NaN");
    elemEq(d.GROUP_BY_COUNT("c", m).values, [2, 0], "an EMPTY group's count is 0");
    elemEq(d.GROUP_BY_SUM("c", "v", m).values, [4, 0],
           "an EMPTY group's SUM stays 0 -- sum is the one identity the scalar publishes");

    const nan = new DataFrame({ k: new Int32Array([0, 1]), v: new Float64Array([NaN, 5]) });
    elemEq(nan.GROUP_BY_MIN("k", "v").values, [Infinity, 5],
           "an ALL-NaN group's min is +Infinity (the seed), exactly as the scalar min is");
    elemEq(nan.GROUP_BY_MAX("k", "v").values, [-Infinity, 5],
           "an ALL-NaN group's max is -Infinity");
    elemEq(nan.GROUP_BY_MEAN("k", "v").values, [NaN, 5], "an ALL-NaN group's mean propagates");
    elemEq(nan.GROUP_BY_COUNT("k").values, [1, 1], "count counts ROWS, NaN included");

    /* infinities are ordinary values here too, and a masked-out one is EXCLUDED
       rather than folded in -- the same pin the scalar family carries */
    const inf = new DataFrame({ k: new Int32Array([0, 0, 0, 0, 0]),
                                v: new Float64Array([1, Infinity, 5, -Infinity, 2]) });
    const im = new Uint8Array([1, 0, 1, 0, 1]);
    elemEq(inf.GROUP_BY_SUM("k", "v", im).values, [8], "masked group sum excludes the infinities");
    elemEq(inf.GROUP_BY_MIN("k", "v", im).values, [1], "masked group min excludes -Infinity");
    elemEq(inf.GROUP_BY_MAX("k", "v", im).values, [5], "masked group max excludes +Infinity");
    elemEq(inf.GROUP_BY_COUNT("k", im).values, [3], "masked group count");
    elemEq(inf.GROUP_BY_MIN("k", "v").values, [-Infinity], "unmasked, -Infinity IS the min");
}

/* ============================================ grouped refusals and the caps */
S("grouped refusals");
{
    const d = new DataFrame({ k: new Int32Array([0, 1]), amt: new Float64Array([1, 2]),
                              city: ["a", "b"] });
    for (const [name, call] of [
        ["GROUP_BY_MIN", (a, b, c) => d.GROUP_BY_MIN(a, b, c)],
        ["GROUP_BY_MAX", (a, b, c) => d.GROUP_BY_MAX(a, b, c)],
        ["GROUP_BY_MEAN", (a, b, c) => d.GROUP_BY_MEAN(a, b, c)],
    ]) {
        throwsLike(() => call("amt", "amt"), "integer or string",
                   name + " refuses a float key column");
        throwsLike(() => call("k", "city"), "string",
                   name + " refuses a string value column");
        throwsLike(() => call("nope", "amt"), "no such column",
                   name + " refuses an unknown key column");
        throwsLike(() => call("k", "nope"), "no such column",
                   name + " refuses an unknown value column");
    }
    throwsLike(() => d.GROUP_BY_COUNT("amt"), "integer or string",
               "GROUP_BY_COUNT refuses a float key column");
    /* GROUP_BY_COUNT has NO value column: the scalar count ignores values, so a
       second name would have nothing to mean. Refused, not reinterpreted as a
       mask -- "mask must be a Uint8Array" would name the wrong mistake. */
    throwsLike(() => d.GROUP_BY_COUNT("k", "amt"), "no value column",
               "GROUP_BY_COUNT refuses a value column instead of guessing");
    throwsLike(() => new DataFrame({ k: new Int32Array([-1]), a: new Float64Array(1) })
                        .GROUP_BY_MIN("k", "a"), "negative",
               "a negative group key is refused by the whole family");
    /* the DF_MAX_GROUPS cap: a hostile key column must not be able to demand an
       unbounded allocation */
    throwsLike(() => new DataFrame({ k: new Int32Array([1 << 20]), a: new Float64Array(1) })
                        .GROUP_BY_MAX("k", "a"), "too many groups",
               "the group-cardinality cap holds for the whole family");
    /* a masked-out row does not launder a malformed key: the cardinality pass
       reads every row */
    throwsLike(() => new DataFrame({ k: new Int32Array([0, -1]), a: new Float64Array(2) })
                        .GROUP_BY_MEAN("k", "a", new Uint8Array([1, 0])), "negative",
               "a negative key is refused even where the mask excludes it");
}

/* ================================== the da82d08 split, for the whole family */
S("grouped empty-key regression guard");
{
    /* An EMPTY string column has an empty dictionary, so the group count is
       legitimately zero. Clamping THAT (rather than only the allocation)
       fabricated a group whose key was read out of a NULL dictionary, and the
       process spun on the faulting instruction forever. A regression here HANGS
       rather than failing, so this breadcrumb is the only thing that names it. */
    console.log("  ... entering the grouped-aggregate empty-dictionary guard");
    for (const [name, call] of [
        ["GROUP_BY_MIN", (d) => d.GROUP_BY_MIN("k", "v")],
        ["GROUP_BY_MAX", (d) => d.GROUP_BY_MAX("k", "v")],
        ["GROUP_BY_MEAN", (d) => d.GROUP_BY_MEAN("k", "v")],
        ["GROUP_BY_COUNT", (d) => d.GROUP_BY_COUNT("k")],
        ["MIN_MAP", (d) => d.MIN_MAP("k", "v")],
        ["MAX_MAP", (d) => d.MAX_MAP("k", "v")],
        ["SUM_MAP", (d) => d.SUM_MAP("k", "v")],
    ]) {
        const g = call(new DataFrame({ k: [], v: new Float64Array(0) }));
        eq(g.keys.length, 0, "empty STRING key column -> zero groups (" + name + ")");
        eq(g.values.length, 0, "and zero values, the same number (" + name + ")");
    }
    /* The INTEGER path agrees now: one counter serves every grouped method, so
       an empty key column reports no groups whichever spelling asked. The
       arithmetic was copied three times and the copies had drifted. */
    const e = new DataFrame({ k: new Int32Array(0), v: new Float64Array(0) });
    for (const name of ["GROUP_BY_MIN", "GROUP_BY_MAX", "GROUP_BY_MEAN",
                        "GROUP_BY_SUM"]) {
        eq(e[name]("k", "v").keys.length, 0,
           "empty INTEGER key column -> zero groups (" + name + ")");
        eq(e[name]("k", "v").values.length, 0,
           "and zero values (" + name + ")");
    }
    eq(e.GROUP_BY_COUNT("k").keys.length, 0, "and none for GROUP_BY_COUNT");

    /* the neighbouring case: a dictionary holding only the empty string */
    const g1 = new DataFrame({ k: [""], v: new Float64Array([7]) });
    eq(g1.GROUP_BY_MIN("k", "v").keys.length, 1, "a dictionary holding only the empty string");
    eq(g1.GROUP_BY_MIN("k", "v").values[0], 7, "and its min");
    eq(g1.GROUP_BY_COUNT("k").values[0], 1, "and its count");
}

/* ===================================== the map-aggregate spelling is an ALIAS */
S("SUM_MAP/MIN_MAP/MAX_MAP");
{
    /* These are the SAME C functions as GROUP_BY_SUM/GROUP_BY_MIN/GROUP_BY_MAX under
       the name the map-aggregate idiom uses. Two names for one implementation
       cannot drift; two implementations would. What is worth testing is only
       that both spellings reach the same code. */
    const df = new DataFrame({ city: ["NY", "SF", "NY", "LA", "SF", "NY"],
                               amt: new Float64Array([1, 2, 3, 4, 5, 6]) });
    const m = df.GT("amt", 2);
    for (const [a, b, name] of [
        ["SUM_MAP", "GROUP_BY_SUM", "SUM_MAP"],
        ["MIN_MAP", "GROUP_BY_MIN", "MIN_MAP"],
        ["MAX_MAP", "GROUP_BY_MAX", "MAX_MAP"],
    ]) {
        for (const mask of [undefined, m]) {
            const x = df[a]("city", "amt", mask), y = df[b]("city", "amt", mask);
            eq(x.keys.join(","), y.keys.join(","), name + " keys match " + b);
            elemEq(x.values, arr(y.values), name + " values match " + b +
                   (mask ? " (masked)" : ""));
        }
        eq(df[a].length, df[b].length, name + " has the same declared arity as " + b);
    }
    /* and they inherit every refusal, because they ARE the same function */
    throwsLike(() => df.MIN_MAP("amt", "amt"), "integer or string", "MIN_MAP inherits the key rule");
    throwsLike(() => df.MAX_MAP("city", "city"), "string", "MAX_MAP inherits the value rule");
    throwsLike(() => df.SUM_MAP("city", "city"), "string", "SUM_MAP inherits the value rule");
    mark("SUM_MAP", "MIN_MAP", "MAX_MAP");
}

/* ============================================================ rolling windows */
S("rolling windows");
{
    const df = new DataFrame({ v: new Float64Array([1, 2, 3, 4]) });
    /* The first w-1 slots are NaN. A partial window would silently compute a
       DIFFERENT statistic -- ROLLING_MEAN(w=3)[0] would be x[0] -- and nothing
       downstream could tell the two apart. */
    elemEq(df.ROLLING_SUM("v", 2), [NaN, 3, 5, 7], "ROLLING_SUM w=2");
    elemEq(df.ROLLING_MEAN("v", 2), [NaN, 1.5, 2.5, 3.5], "ROLLING_MEAN w=2");
    elemEq(df.ROLLING_MIN("v", 2), [NaN, 1, 2, 3], "ROLLING_MIN w=2");
    elemEq(df.ROLLING_MAX("v", 2), [NaN, 2, 3, 4], "ROLLING_MAX w=2");
    elemEq(df.ROLLING_SUM("v", 3), [NaN, NaN, 6, 9], "ROLLING_SUM w=3");
    elemEq(df.ROLLING_MEAN("v", 3), [NaN, NaN, 2, 3], "ROLLING_MEAN w=3");
    elemEq(df.ROLLING_MIN("v", 3), [NaN, NaN, 1, 2], "ROLLING_MIN w=3");
    elemEq(df.ROLLING_MAX("v", 3), [NaN, NaN, 3, 4], "ROLLING_MAX w=3");

    /* w == 1 is the identity for sum and mean, and it is NOT for min/max on a
       NaN row: one row counted, the NaN was ignored, so the seed surfaces --
       the same answer the scalar min gives for a one-row all-NaN column. */
    elemEq(df.ROLLING_SUM("v", 1), [1, 2, 3, 4], "w=1 sum is the column");
    elemEq(df.ROLLING_MEAN("v", 1), [1, 2, 3, 4], "w=1 mean is the column");
    const dn = new DataFrame({ v: new Float64Array([1, NaN, 3]) });
    elemEq(dn.ROLLING_MIN("v", 1), [1, Infinity, 3],
           "a w=1 window over a NaN row surfaces the seed, like the scalar min");
    elemEq(dn.ROLLING_MAX("v", 1), [1, -Infinity, 3], "and -Infinity for max");
    /* NaN inside a wider window is IGNORED by min/max and PROPAGATES through
       sum/mean -- the two families disagree here exactly as they do scalar */
    elemEq(dn.ROLLING_MIN("v", 2), [NaN, 1, 3], "ROLLING_MIN ignores a NaN in the window");
    elemEq(dn.ROLLING_MAX("v", 2), [NaN, 1, 3], "ROLLING_MAX ignores a NaN in the window");
    elemEq(dn.ROLLING_SUM("v", 2), [NaN, NaN, NaN], "ROLLING_SUM propagates it");
    elemEq(dn.ROLLING_MEAN("v", 2), [NaN, NaN, NaN], "ROLLING_MEAN propagates it");

    /* a window as long as the column fills exactly one slot; longer than the
       column is not an error, it simply never fills */
    elemEq(df.ROLLING_SUM("v", 4), [NaN, NaN, NaN, 10], "w == rows fills exactly one slot");
    elemEq(df.ROLLING_SUM("v", 5), [NaN, NaN, NaN, NaN], "w > rows is legal and all NaN");
    elemEq(df.ROLLING_MIN("v", 1000), [NaN, NaN, NaN, NaN], "and so is a much longer one");

    /* the result is always `rows` long, so it lines up with the frame it came
       from and can be handed straight back to `new DataFrame` */
    eq(df.ROLLING_SUM("v", 2).length, df.ROWS, "a rolling result is `rows` long");
    eq(new DataFrame({ r: df.ROLLING_SUM("v", 3) }).ROWS, df.ROWS,
       "and can be handed straight back to the constructor");
    eq(df.ROLLING_SUM("v", 2).constructor, Float64Array, "rolling returns a Float64Array");

    /* every numeric column type feeds it */
    for (const [tag, T] of INTS) {
        const d = new DataFrame({ v: new T([1, 2, 3]) });
        elemEq(d.ROLLING_SUM("v", 2), [NaN, 3, 5], "ROLLING_SUM over a " + tag + " column");
    }
    eq(new DataFrame({ v: new Float64Array(0) }).ROLLING_SUM("v", 3).length, 0,
       "rolling over zero rows -> length 0");
    throwsLike(() => new DataFrame({ c: ["a"], v: new Float64Array(1) }).ROLLING_SUM("c", 1),
               "string", "rolling refuses a string column");
    mark("ROLLING_SUM", "ROLLING_MEAN", "ROLLING_MIN", "ROLLING_MAX");
}

/* =============================================== the window argument itself */
S("rolling window argument");
{
    const df = new DataFrame({ v: new Float64Array([1, 2, 3, 4]) });
    /* A window of zero has no statistic -- every slot would come back 0 or NaN
       and look like data. Coerced and validated in DOUBLE and once per call:
       JS_ToInt32 would have truncated 2.5 to 2 and wrapped 4294967297 to 1,
       both plausible wrong answers from a window the caller computed. */
    for (const [name, call] of [
        ["ROLLING_SUM", (w) => df.ROLLING_SUM("v", w)],
        ["ROLLING_MEAN", (w) => df.ROLLING_MEAN("v", w)],
        ["ROLLING_MIN", (w) => df.ROLLING_MIN("v", w)],
        ["ROLLING_MAX", (w) => df.ROLLING_MAX("v", w)],
    ]) {
        throwsLike(() => call(0), "positive integer", name + " refuses window 0");
        throwsLike(() => call(-1), "positive integer", name + " refuses a negative window");
        throwsLike(() => call(2.5), "positive integer", name + " refuses a fractional window");
        throwsLike(() => call(NaN), "positive integer", name + " refuses a NaN window");
        throwsLike(() => call(Infinity), "positive integer", name + " refuses an infinite window");
        throwsLike(() => call(4294967297), "positive integer",
                   name + " refuses 2^32+1 rather than wrapping it to 1");
        throwsLike(() => call(undefined), "positive integer",
                   name + " refuses a missing window");
        eq(call(2).length, 4, name + " still works after every refusal above");
    }
    /* the refusal names which method it came from */
    throwsLike(() => df.ROLLING_MEAN("v", 0), "ROLLING_MEAN",
               "the window refusal names the method");
}

/* ======================================== the mask picks CONTRIBUTORS, always */
S("rolling and the mask");
{
    /* A masked-out row contributes nothing but still occupies its output slot.
       Anything else would make the mask mean something different here from what
       it means in every other method in this module. */
    const df = new DataFrame({ v: new Float64Array([1, 2, 3]) });
    const m = new Uint8Array([1, 0, 1]);
    elemEq(df.ROLLING_SUM("v", 2, m), [NaN, 1, 3],
           "a masked-out row leaves the window, not the output");
    elemEq(df.ROLLING_MEAN("v", 2, m), [NaN, 1, 3],
           "ROLLING_MEAN divides by what CONTRIBUTED, never by w");
    elemEq(df.ROLLING_MIN("v", 2, m), [NaN, 1, 3], "ROLLING_MIN over the same mask");
    elemEq(df.ROLLING_MAX("v", 2, m), [NaN, 1, 3], "ROLLING_MAX over the same mask");

    /* a window with nothing left in it: min/max are NaN (the count==0 case the
       scalar answers with `undefined`), sum is 0 and mean is NaN -- the same
       three answers an EMPTY COLUMN gets from the scalar family */
    const m2 = new Uint8Array([1, 0, 0]);
    elemEq(df.ROLLING_SUM("v", 2, m2), [NaN, 1, 0], "an emptied window sums to 0");
    elemEq(df.ROLLING_MEAN("v", 2, m2), [NaN, 1, NaN], "an emptied window means NaN");
    elemEq(df.ROLLING_MIN("v", 2, m2), [NaN, 1, NaN], "an emptied window mins NaN");
    elemEq(df.ROLLING_MAX("v", 2, m2), [NaN, 1, NaN], "an emptied window maxes NaN");
    /* a masked-out infinity is EXCLUDED, not folded in as a value */
    const inf = new DataFrame({ v: new Float64Array([1, Infinity, 5, -Infinity, 2]) });
    const im = new Uint8Array([1, 0, 1, 0, 1]);
    elemEq(inf.ROLLING_SUM("v", 5, im), [NaN, NaN, NaN, NaN, 8], "masked-out infinities excluded");
    elemEq(inf.ROLLING_MIN("v", 5, im), [NaN, NaN, NaN, NaN, 1], "min excludes -Infinity");
    elemEq(inf.ROLLING_MAX("v", 5, im), [NaN, NaN, NaN, NaN, 5], "max excludes +Infinity");
    throwsLike(() => df.ROLLING_SUM("v", 2, new Uint8Array(1)), "Uint8Array",
               "a mask shorter than the frame is refused");
}

/* ================================= rolling against the definition, swept ==== */
S("rolling sweep");
{
    /* The definition, written out slowly in JS. Not a second copy of the
       kernel: it re-reads the whole window at every position on purpose, which
       is what the C is specified to compute. The lengths bracket the w-1
       boundary from both sides, and the mask alternates so no window is either
       wholly present or wholly absent by luck. */
    function ref(x, w, m, kind) {
        const n = x.length, out = new Array(n).fill(NaN);
        for (let i = 0; i < n; i++) {
            if (i < w - 1) continue;
            let s = 0, c = 0, lo = Infinity, hi = -Infinity;
            for (let j = i - w + 1; j <= i; j++) {
                if (m && !m[j]) continue;
                const v = x[j];
                s += v; c++;
                if (v < lo) lo = v;
                if (v > hi) hi = v;
            }
            out[i] = kind === "SUM" ? s
                   : kind === "MEAN" ? (c ? s / c : NaN)
                   : kind === "MIN" ? (c ? lo : NaN)
                   : (c ? hi : NaN);
        }
        return out;
    }
    let cells = 0;
    for (const n of [0, 1, 2, 3, 5, 9, 17]) {
        const x = new Float64Array(n);
        for (let i = 0; i < n; i++) x[i] = ((i * 37) % 19) - 9 + (i % 5 === 3 ? 0.5 : 0);
        if (n > 4) x[n - 2] = NaN;                 /* a NaN in the tail window */
        const mask = new Uint8Array(n);
        for (let i = 0; i < n; i++) mask[i] = (i % 3) !== 1;
        const df = new DataFrame({ x });
        for (const w of [1, 2, 3, 4, 8, 17]) {
            for (const kind of ["SUM", "MEAN", "MIN", "MAX"]) {
                elemEq(df["ROLLING_" + kind]("x", w), ref(x, w, null, kind),
                       "ROLLING_" + kind + " n=" + n + " w=" + w);
                elemEq(df["ROLLING_" + kind]("x", w, mask), ref(x, w, mask, kind),
                       "ROLLING_" + kind + " n=" + n + " w=" + w + " masked");
                cells += 2;
            }
        }
    }
    ok(cells === 7 * 6 * 4 * 2, "the rolling sweep covered every (n, w, aggregate)",
       cells + " cells");
}

/* ============================== grouped aggregates against the definition ==== */
S("grouped sweep");
{
    /* A Map-based reference, which is how the same query would be written in
       JS. It also answers the empty group differently ON PURPOSE -- it never
       creates one -- so the dense-integer expectation is written out here. */
    function ref(keys, vals, mask, kind, ngroups) {
        const s = new Array(ngroups).fill(0), c = new Array(ngroups).fill(0);
        const lo = new Array(ngroups).fill(Infinity);
        const hi = new Array(ngroups).fill(-Infinity);
        for (let i = 0; i < keys.length; i++) {
            if (mask && !mask[i]) continue;
            const g = keys[i], v = vals[i];
            s[g] += v; c[g]++;
            if (v < lo[g]) lo[g] = v;
            if (v > hi[g]) hi[g] = v;
        }
        const out = new Array(ngroups);
        for (let g = 0; g < ngroups; g++)
            out[g] = kind === "SUM" ? s[g]
                   : kind === "MEAN" ? (c[g] ? s[g] / c[g] : NaN)
                   : kind === "MIN" ? (c[g] ? lo[g] : NaN)
                   : kind === "MAX" ? (c[g] ? hi[g] : NaN)
                   : c[g];
        return out;
    }
    const NG = 4;
    for (const n of [1, 5, 17, 64]) {
        const k = new Int32Array(n), v = new Float64Array(n), mask = new Uint8Array(n);
        for (let i = 0; i < n; i++) {
            k[i] = (i * 7) % NG;
            v[i] = ((i * 13) % 23) - 11;
            mask[i] = i % 4 !== 2;
        }
        if (n > 8) v[3] = NaN;
        /* group NG-1 is emptied outright at n=5 by this mask, which is the
           case the reference and the module have to agree on */
        const ng = Math.max(...Array.from(k)) + 1;
        const df = new DataFrame({ k, v });
        for (const kind of ["SUM", "MEAN", "MIN", "MAX"]) {
            elemEq(df["GROUP_BY_" + kind]("k", "v").values, ref(k, v, null, kind, ng),
                   "GROUP_BY_" + kind + " n=" + n);
            elemEq(df["GROUP_BY_" + kind]("k", "v", mask).values, ref(k, v, mask, kind, ng),
                   "GROUP_BY_" + kind + " n=" + n + " masked");
        }
        elemEq(df.GROUP_BY_COUNT("k").values, ref(k, v, null, "Count", ng),
               "GROUP_BY_COUNT n=" + n);
        elemEq(df.GROUP_BY_COUNT("k", mask).values, ref(k, v, mask, "Count", ng),
               "GROUP_BY_COUNT n=" + n + " masked");
    }
}

/* ======================================================================= dropna */
S("DROP_NA");
{
    /* A MASK, not a compacted column. Compacting changes the row count, so the
       result could not be handed back to the frame it came from, and one
       returned array cannot compact the other columns anyway. The mask, in
       exchange, composes with every reduction here -- which is the test below
       that matters most. */
    const df = new DataFrame({
        a: new Float64Array([1, NaN, 3]),
        b: new Float64Array([1, 2, NaN]),
        i: new Int32Array([1, 2, 3]),
        f: new Float32Array([1, 2, NaN]),
        s: ["x", "y", "z"],
    });
    eq(df.DROP_NA("a").constructor, Uint8Array, "dropna returns a Uint8Array mask");
    eq(df.DROP_NA("a").length, df.ROWS, "one byte per row, like every other mask here");
    elemEq(df.DROP_NA("a"), [1, 0, 1], "dropna(a)");
    elemEq(df.DROP_NA("b"), [1, 1, 0], "dropna(b)");
    elemEq(df.DROP_NA("f"), [1, 1, 0], "a Float32Array NaN is found too");
    elemEq(df.DROP_NA("i"), [1, 1, 1], "an integer column can never be NA");

    /* the multi-column AND is the capability. Over ONE column dropna would be
       `notna` under a second name, and there is no elementwise mask-AND in this
       module to build the conjunction from. */
    elemEq(df.DROP_NA("a", "b"), [1, 0, 0], "dropna(a, b) is the AND of both");
    elemEq(df.DROP_NA("a", "b", "i"), [1, 0, 0], "an integer column adds nothing");
    elemEq(df.DROP_NA("a", "a"), [1, 0, 1], "naming a column twice is idempotent");
    elemEq(df.DROP_NA("a"), arr(df.NOT_NA("a")), "over one column it agrees with notna");

    /* no arguments: every NUMERIC column. A string column is SKIPPED there -- a
       dictionary code is never NaN, and refusing would make the no-argument
       form useless on any frame holding one -- but REFUSED when named, because
       naming it asserts it can be missing. */
    elemEq(df.DROP_NA(), [1, 0, 0], "dropna() is every numeric column, string columns skipped");
    throwsLike(() => df.DROP_NA("s"), "string", "a NAMED string column is refused");
    throwsLike(() => df.DROP_NA("nope"), "no such column", "an unknown column is refused");
    throwsLike(() => df.DROP_NA("a", "nope"), "no such column",
               "and so is an unknown column in second place");
    elemEq(new DataFrame({ s: ["x", "y"] }).DROP_NA(), [1, 1],
           "dropna() over no numeric columns at all is vacuously all-keep");
    eq(new DataFrame({ a: new Float64Array(0) }).DROP_NA().length, 0,
       "dropna over zero rows -> length 0");

    /* THE REASON IT IS A MASK: it composes */
    eq(df.SUM("a", df.DROP_NA("a")), 4, "the dropna mask composes into sum");
    eq(df.MEAN("a", df.DROP_NA("a")), 2, "and into mean, which is now not NaN");
    eq(df.COUNT("a", df.DROP_NA("a")), 2, "and into count");
    eq(df.MIN("a", df.DROP_NA("a")), 1, "and into min");
    eq(df.SUM("a", df.DROP_NA("a", "b")), 1, "and the conjunction narrows it further");
    eq(arr(df.GROUP_BY_SUM("s", "a", df.DROP_NA("a")).values).join(","), "1,0,3",
       "and into a grouped reduction");
    mark("DROP_NA");
}

/* -------------------------------------------------- [G7-B] memory safety
   Insert these three inside S("memory safety"), after attack 10 and before the
   `ok(hooksFired >= 10, ...)` line. Each PROVES ITS HOOK FIRED: the measured
   mapping is column NAME -> toString, any NUMBER -> valueOf, and a mask ->
   neither, so an attack aimed at the wrong coercion passes having run nothing.
   ------------------------------------------------------------------------- */

{   /* 11. the grouped family's value-column name, after the key is resolved */
    attack("detach during GROUP_BY_MIN's SECOND column name", (fire) => {
        const buf = new ArrayBuffer(8 * 8);
        const v = new Float64Array(buf);
        const df = new DataFrame({ k: new Int32Array(8), v });
        return df.GROUP_BY_MIN("k", { toString() { fire(); buf.transfer(); return "v"; } });
    });
}
{   /* 12. rolling's WINDOW is a number, so it is valueOf and not toString -- an
       attack written against toString here would run nothing at all. The window
       is coerced before any column is bound, which is what this checks. */
    attack("detach during ROLLING_SUM's WINDOW coercion (valueOf)", (fire) => {
        const buf = new ArrayBuffer(8 * 8);
        const v = new Float64Array(buf);
        const df = new DataFrame({ v });
        return df.ROLLING_SUM("v", { valueOf() { fire(); buf.transfer(); return 2; } });
    });
}
{   /* 13. dropna resolves EVERY name before binding anything, so a detach from
       the second name must be caught -- this is the one that fails if the loop
       ever binds as it goes. */
    attack("detach during dropna's SECOND column name", (fire) => {
        const buf = new ArrayBuffer(8 * 8);
        const v = new Float64Array(buf);
        const df = new DataFrame({ a: new Float64Array(8), v });
        return df.DROP_NA("a", { toString() { fire(); buf.transfer(); return "v"; } });
    });
}

/* -------------------------------------------------- [G7-C] already detached
   Insert next to the existing `GROUP_BY_SUM on a detached buffer` line, inside
   the same block (it has `df` over a detached `v`).
   ------------------------------------------------------------------------- */

    throwsLike(() => df.GROUP_BY_MIN("v", "v"), "detached", "GROUP_BY_MIN on a detached buffer");
    throwsLike(() => df.GROUP_BY_MAX("v", "v"), "detached", "GROUP_BY_MAX on a detached buffer");
    throwsLike(() => df.GROUP_BY_MEAN("v", "v"), "detached", "GROUP_BY_MEAN on a detached buffer");
    throwsLike(() => df.GROUP_BY_COUNT("v"), "detached", "GROUP_BY_COUNT on a detached buffer");
    throwsLike(() => df.ROLLING_SUM("v", 2), "detached", "ROLLING_SUM on a detached buffer");
    throwsLike(() => df.ROLLING_MEAN("v", 2), "detached", "ROLLING_MEAN on a detached buffer");
    throwsLike(() => df.ROLLING_MIN("v", 2), "detached", "ROLLING_MIN on a detached buffer");
    throwsLike(() => df.ROLLING_MAX("v", 2), "detached", "ROLLING_MAX on a detached buffer");
    throwsLike(() => df.DROP_NA("v"), "detached", "dropna on a detached buffer");
    throwsLike(() => df.DROP_NA(), "detached", "dropna() on a detached buffer");
    throwsLike(() => df.SUM_MAP("v", "v"), "detached", "SUM_MAP on a detached buffer");
    throwsLike(() => df.MIN_MAP("v", "v"), "detached", "MIN_MAP on a detached buffer");
    throwsLike(() => df.MAX_MAP("v", "v"), "detached", "MAX_MAP on a detached buffer");

/* ================ cardinality and sketches (families whose generators died) ==
   These twelve methods shipped without their generated test sections, so the
   coverage below is written against the CONTRACT rather than lifted: distinct
   values counted by presence not by dictionary size, count-descending
   VALUE_COUNTS with TOP_K as its prefix, and sketches asserted against their
   exact counterparts in the same module rather than against hand-written
   expectations. */
S("cardinality: unique / nunique / mode / DROP_DUPLICATES");
mark("UNIQUE", "N_UNIQUE", "MODE", "DROP_DUPLICATES");
{
    const v = Float64Array.from([3, 1, 4, 1, 5, 9, 2, 6, 5, 3]);
    const k = ["a", "b", "a", "c", "b", "a", "c", "b", "a", "c"];
    const d = new DataFrame({ v, k });

    elemEq(d.UNIQUE("v"), [3, 1, 4, 5, 9, 2, 6], "unique is first-seen, not sorted");
    eq(d.N_UNIQUE("v"), 7, "nunique counts distinct values");
    eq(d.UNIQUE("k").join(","), "a,b,c", "unique on a string column returns STRINGS, not codes");
    eq(d.N_UNIQUE("k"), 3, "nunique on a string column");
    eq(d.MODE("v"), 3, "mode breaks a tie by first appearance (3, 1 and 5 all appear twice)");
    eq(d.MODE("k"), "a", "mode of a string column returns a string");

    const dd = d.DROP_DUPLICATES("v");
    eq(dd.length, d.ROWS, "DROP_DUPLICATES returns an nrows-long mask, so it composes");
    eq(d.COUNT("v", dd), d.N_UNIQUE("v"),
       "the identity that ties the two: counting under the mask gives nunique");

    /* the mask case is the whole reason nunique counts codes PRESENT rather than
       dictionary size -- masking a city out must drop the count */
    const noA = Uint8Array.from(k.map(x => x === "a" ? 0 : 1));
    eq(d.N_UNIQUE("k", noA), 2, "masking every 'a' row drops nunique by one");

    eq(new DataFrame({ e: new Float64Array(0) }).N_UNIQUE("e"), 0, "nunique of an empty column");
}

S("cardinality: VALUE_COUNTS / TOP_K / GROUP_ARRAY");
mark("VALUE_COUNTS", "TOP_K", "GROUP_ARRAY", "GROUP_UNIQ_ARRAY");
{
    const k = ["a", "b", "a", "c", "b", "a", "c", "b"];
    const v = Float64Array.from([1, 2, 3, 4, 5, 6, 7, 8]);
    const d = new DataFrame({ k, v });

    const vc = d.VALUE_COUNTS("k");
    eq(vc.keys.join(","), "a,b,c", "VALUE_COUNTS is count-descending, ties by first appearance");
    elemEq(vc.values, [3, 3, 2], "VALUE_COUNTS counts");

    const tk = d.TOP_K("k", 2);
    eq(tk.keys.join(","), "a,b", "TOP_K is the prefix of VALUE_COUNTS");
    elemEq(tk.values, [3, 3], "TOP_K counts agree with VALUE_COUNTS");
    eq(d.TOP_K("k", 99).keys.length, 3, "an oversized k clamps to the distinct count");

    const ga = d.GROUP_ARRAY("k", "v");
    eq(ga.keys.join(","), d.GROUP_BY_SUM("k", "v").keys.join(","),
       "GROUP_ARRAY keys are element-for-element GROUP_BY_SUM's keys");
    let total = 0;
    for (const g of ga.values) for (const x of g) total += x;
    eq(total, 36, "GROUP_ARRAY partitions the column without losing or duplicating a value");

    const d3 = new DataFrame({ k, w: Float64Array.from([1, 1, 1, 2, 2, 1, 2, 2]) });
    const gu = d3.GROUP_UNIQ_ARRAY("k", "w");
    eq(gu.values[0].length, 1, "GROUP_UNIQ_ARRAY deduplicates within a group");
}

S("sketches: approximate answers checked against their exact counterparts");
mark("APPROX_COUNT_DISTINCT", "APPROX_PERCENTILE", "APPROX_TOP_K", "APPROX_SIMILARITY");
{
    const n = 20000;
    const v = new Float64Array(n);
    for (let i = 0; i < n; i++) v[i] = i % 4096;      /* 4096 distinct */
    const d = new DataFrame({ v });

    const exact = d.N_UNIQUE("v");
    eq(exact, 4096, "the exact distinct count, as the sketch's reference");
    const est = d.APPROX_COUNT_DISTINCT("v");
    ok(Math.abs(est - exact) / exact <= 0.05,
       "APPROX_COUNT_DISTINCT is within its documented 5% bound",
       "estimate " + est + " vs exact " + exact);

    /* an order statistic: assert the rank of what came back, not its value --
       that is the bound the sketch actually offers */
    const q = d.APPROX_PERCENTILE("v", 0.5);
    const below = d.COUNT("v", d.LT("v", q)) / n;
    ok(Math.abs(below - 0.5) <= 0.02,
       "APPROX_PERCENTILE's answer sits at the requested RANK, within bound",
       "fraction below = " + below);

    const k2 = ["x", "x", "x", "x", "y", "y", "z"];
    const d2 = new DataFrame({ k: k2 });
    const at = d2.APPROX_TOP_K("k", 2);
    eq(at.keys.join(","), d2.TOP_K("k", 2).keys.join(","),
       "APPROX_TOP_K agrees with exact TOP_K where the sketch is exact");

    const a = new DataFrame({ p: Float64Array.from([1, 2, 3, 4]), q: Float64Array.from([1, 2, 3, 4]) });
    near(a.APPROX_SIMILARITY("p", "q"), 1,
         "APPROX_SIMILARITY of a column with itself is 1, and is EXACT below the sketch width", 1e-9);
    const b = new DataFrame({ p: Float64Array.from([1, 2]), q: Float64Array.from([3, 4]) });
    eq(b.APPROX_SIMILARITY("p", "q"), 0, "disjoint columns give Jaccard 0");
}

/* ======================================== additional aggregates (21) ===== */
S("additional aggregates: moments and regression sums");
mark("SEM", "SKEW_SAMP", "KURT_SAMP", "COUNT_NULLS",
     "REGR_COUNT", "REGR_SXX", "REGR_SYY", "REGR_SXY");
{
    const v = Float64Array.from([1, 2, 3, 4, 5, 6, 7, 8, 9, 20]);
    const w = Float64Array.from([1, 1, 2, 2, 1, 1, 2, 2, 1, 1]);
    const d = new DataFrame({ v, w });
    const n = 10;

    /* the sample forms, against the textbook adjustment of the population ones */
    let mean = 0; for (const x of v) mean += x; mean /= n;
    let m2 = 0, m3 = 0, m4 = 0;
    for (const x of v) { const q = x - mean; m2 += q*q; m3 += q*q*q; m4 += q*q*q*q; }
    const g1 = (m3/n)/Math.pow(m2/n, 1.5), g2 = (m4/n)/Math.pow(m2/n, 2) - 3;
    near(d.SKEW_SAMP("v"), g1*Math.sqrt(n*(n-1))/(n-2), "SKEW_SAMP is the adjusted g1", 1e-12);
    near(d.KURT_SAMP("v"), ((n+1)*g2+6)*(n-1)/((n-2)*(n-3)), "KURT_SAMP is the adjusted g2", 1e-12);
    ok(d.SKEW_SAMP("v") !== d.SKEW("v"), "SKEW_SAMP differs from the population SKEW");
    ok(d.KURT_SAMP("v") !== d.KURTOSIS("v"), "KURT_SAMP differs from the population KURTOSIS");

    /* SEM is the SAMPLE stddev over sqrt(n) -- tie it to stddev(), not a constant */
    near(d.SEM("v"), d.STDDEV("v")/Math.sqrt(n), "SEM == STDDEV/sqrt(n)", 1e-12);

    const nn = new DataFrame({ a: Float64Array.from([1, NaN, 3, NaN, 5]) });
    eq(nn.COUNT_NULLS("a"), 2, "COUNT_NULLS counts the NaN rows");
    eq(nn.COUNT_NULLS("a") + nn.COUNT("a", nn.NOT_NA("a")), 5,
       "nulls plus non-nulls is every row");
    eq(new DataFrame({ e: new Float64Array(0) }).COUNT_NULLS("e"), 0,
       "COUNT_NULLS of an empty column");

    /* the regression sums must be the ones the fitted family already uses */
    eq(d.REGR_COUNT("v", "w"), n, "REGR_COUNT is the paired row count");
    near(d.REGR_SXY("v", "w") / d.REGR_SXX("v", "w"), d.REGR_SLOPE("v", "w"),
         "SXY/SXX is exactly REGR_SLOPE", 1e-12);
    near(d.REGR_SXX("v", "w"), d.VARIANCE_POP("w") * n,
         "SXX is the INDEPENDENT column's spread, i.e. the second argument", 1e-9);
    near(d.REGR_SYY("v", "w"), d.VARIANCE_POP("v") * n,
         "SYY is the DEPENDENT column's spread", 1e-9);
}

S("additional aggregates: deviation and entropy");
mark("MAD", "MEDIAN_ABSOLUTE_DEVIATION", "ENTROPY");
{
    /* MAD centres on the MEAN, MEDIAN_ABSOLUTE_DEVIATION on the MEDIAN -- an
       asymmetric column separates them, which a symmetric one would not */
    const d = new DataFrame({ v: Float64Array.from([1, 2, 3, 4, 5, 6, 7, 8, 9, 20]) });
    near(d.MAD("v"), 3.6, "MAD is the mean absolute deviation", 1e-12);
    near(d.MEDIAN_ABSOLUTE_DEVIATION("v"), 2.5, "MEDIAN_ABSOLUTE_DEVIATION", 1e-12);
    ok(d.MAD("v") !== d.MEDIAN_ABSOLUTE_DEVIATION("v"),
       "the two deviations are different statistics, not aliases");

    /* two equally likely values is exactly one bit; a constant column is zero */
    near(new DataFrame({ e: Float64Array.from([1,1,2,2]) }).ENTROPY("e"), 1,
         "ENTROPY of a fair two-value column is 1 bit", 1e-12);
    near(new DataFrame({ e: Float64Array.from([7,7,7,7]) }).ENTROPY("e"), 0,
         "ENTROPY of a constant column is 0", 1e-12);
    near(new DataFrame({ e: Float64Array.from([1,2,3,4]) }).ENTROPY("e"), 2,
         "four equally likely values is 2 bits", 1e-12);
    eq(new DataFrame({ e: new Float64Array(0) }).ENTROPY("e"), 0, "ENTROPY of nothing");
    /* NaN is a VALUE here, as it is everywhere in the cardinality family, so a
       column of two ones and two NaNs is a fair two-way split, not a constant */
    near(new DataFrame({ e: Float64Array.from([1,1,NaN,NaN]) }).ENTROPY("e"), 1,
         "NaN counts as one distinct value, matching VALUE_COUNTS", 1e-12);
    eq(new DataFrame({ e: Float64Array.from([1,1,NaN,NaN]) }).VALUE_COUNTS("e").keys.length, 2,
       "and VALUE_COUNTS agrees there are two values");
}

S("additional aggregates: quantile variants and histogram");
mark("QUANTILE_EXACT_LOW", "QUANTILE_EXACT_HIGH", "QUANTILES", "QUANTILES_TDIGEST",
     "UNIQ_UP_TO", "HISTOGRAM", "HISTOGRAM_NORMALIZED");
{
    const d = new DataFrame({ v: Int32Array.from([1, 2, 3, 4]) });
    /* both must return a value PRESENT in the column, unlike PERCENTILE_CONT */
    eq(d.QUANTILE_EXACT_LOW("v", 0.5), 2, "QUANTILE_EXACT_LOW takes the lower");
    eq(d.QUANTILE_EXACT_HIGH("v", 0.5), 3, "QUANTILE_EXACT_HIGH takes the upper");
    eq(d.PERCENTILE_CONT("v", 0.5), 2.5, "PERCENTILE_CONT interpolates between them");
    throwsLike(() => d.QUANTILE_EXACT_LOW("v", 2), "[0, 1]", "an out-of-range q throws");

    const w = new DataFrame({ v: Float64Array.from([1,2,3,4,5,6,7,8,9,20]) });
    const qs = w.QUANTILES("v", [0, 0.25, 0.5, 0.75, 1]);
    eq(qs.length, 5, "QUANTILES returns one value per q");
    /* every entry must equal the single-quantile call: one sort, same answers */
    for (const [i, q] of [0, 0.25, 0.5, 0.75, 1].entries())
        near(qs[i], w.QUANTILE("v", q), "QUANTILES[" + i + "] == QUANTILE(" + q + ")", 1e-12);
    throwsLike(() => w.QUANTILES("v", [0.5, 2]), "[0, 1]", "QUANTILES validates every q");

    /* QUANTILES_TDIGEST reads N quantiles off ONE approximate digest. On a large
       uniform column it must track the EXACT quantile closely, and each entry
       must equal the single APPROX_PERCENTILE call -- one digest, same answers. */
    {
        const big = new Float64Array(10000);
        for (let i = 0; i < big.length; i++) big[i] = i;
        const bd = new DataFrame({ v: big });
        const probe = [0.1, 0.5, 0.9, 0.99];
        const td = bd.QUANTILES_TDIGEST("v", probe);
        eq(td.length, probe.length, "QUANTILES_TDIGEST returns one value per q");
        for (const [i, q] of probe.entries()) {
            near(td[i], bd.QUANTILE("v", q),
                 "QUANTILES_TDIGEST[" + i + "] tracks the exact quantile", 60);
            near(td[i], bd.APPROX_PERCENTILE("v", q),
                 "and equals the single-digest APPROX_PERCENTILE", 1e-9);
        }
        throwsLike(() => bd.QUANTILES_TDIGEST("v", [0.5, 2]), "[0, 1]",
                   "QUANTILES_TDIGEST validates every q");
        eq(new DataFrame({ e: new Float64Array(0) }).QUANTILES_TDIGEST("e", [0.5])[0] !== undefined
           ? Number.isNaN(new DataFrame({ e: new Float64Array(0) }).QUANTILES_TDIGEST("e", [0.5])[0]) : false,
           true, "an empty column gives NaN per quantile");
    }

    eq(w.UNIQ_UP_TO("v", 5), 6, "UNIQ_UP_TO stops one past the cap");
    eq(w.UNIQ_UP_TO("v", 100), 10, "under the cap it is the exact distinct count");
    eq(w.UNIQ_UP_TO("v", 100), w.N_UNIQUE("v"), "and agrees with N_UNIQUE there");
    /* the two must agree on the SameValueZero rules too, not just on plain
       columns: NaN is one distinct value and -0 collapses into +0 */
    for (const c of [[1,1,2,NaN,3,-0,0], [NaN,NaN], [0,-0], [5], [], [1,2,3,4,5]]) {
        const dd = new DataFrame({ a: Float64Array.from(c) });
        eq(dd.UNIQ_UP_TO("a", 1000), dd.N_UNIQUE("a"),
           "UNIQ_UP_TO agrees with N_UNIQUE on [" + c.join(",") + "]");
    }

    const h = w.HISTOGRAM("v", 4);
    eq(h.edges.length, 5, "HISTOGRAM has bins+1 edges");
    eq(h.counts.length, 4, "and one count per bin");
    let tot = 0; for (const c of h.counts) tot += c;
    eq(tot, 10, "every non-NaN row lands in exactly one bin");
    eq(h.counts[3], 1, "the top edge is inclusive, so the maximum is not lost");
    const hn = w.HISTOGRAM_NORMALIZED("v", 4);
    let s = 0; for (const c of hn.counts) s += c;
    near(s, 1, "HISTOGRAM_NORMALIZED sums to one", 1e-12);
    throwsLike(() => w.HISTOGRAM("v", 0), "positive integer", "zero bins is refused");
    const flat = new DataFrame({ c: Float64Array.from([5,5,5]) });
    eq(flat.HISTOGRAM("c", 3).counts[0], 3, "a zero-width range does not divide by zero");
}

S("additional aggregates: time series");
mark("EMA", "DELTA_SUM", "DELTA_SUM_TIMESTAMP", "RATE", "IRATE");
{
    const d = new DataFrame({
        v: Float64Array.from([1, 2, 3, 4, 5, 6, 7, 8, 9, 20]),
        t: Float64Array.from([0, 1, 2, 3, 4, 5, 6, 7, 8, 10]),
    });
    const e = d.EMA("v", 0.5);
    eq(e.length, d.ROWS, "EMA is `rows` long, like the scans");
    eq(e[0], 1, "EMA seeds on the first selected value");
    near(e[1], 1.5, "and then blends", 1e-12);
    eq(d.EMA("v", 1)[3], 4, "alpha of 1 is the value itself");
    throwsLike(() => d.EMA("v", 0), "(0, 1]", "alpha of zero is refused");
    throwsLike(() => d.EMA("v", 2), "(0, 1]", "alpha above one is refused");

    /* only the RISES count, which is what makes it a counter total */
    eq(new DataFrame({ c: Float64Array.from([1,5,2,7]) }).DELTA_SUM("c"), 9,
       "DELTA_SUM adds 4 and 5, ignoring the drop");
    eq(new DataFrame({ c: Float64Array.from([9,8,7]) }).DELTA_SUM("c"), 0,
       "a monotonically falling column has no positive delta");

    /* DELTA_SUM_TIMESTAMP sums the rises in TIME order, not row order, so an
       out-of-order frame is handled: times 3,1,2,4 with values 10,5,8,6 sort to
       values 5,8,10,6, whose rises are 3 and 2. Row order gives a different 3. */
    const ts = new DataFrame({
        t: Float64Array.from([3, 1, 2, 4]),
        x: Float64Array.from([10, 5, 8, 6]),
    });
    eq(ts.DELTA_SUM("x"), 3, "DELTA_SUM in ROW order sums only 8>5");
    eq(ts.DELTA_SUM_TIMESTAMP("x", "t"), 5, "DELTA_SUM_TIMESTAMP sorts by time: 3+2");
    ok(ts.DELTA_SUM("x") !== ts.DELTA_SUM_TIMESTAMP("x", "t"),
       "the two disagree on an out-of-order column, which is the whole point");
    eq(new DataFrame({ a: new Float64Array(0), b: new Float64Array(0) })
       .DELTA_SUM_TIMESTAMP("a", "b"), 0, "an empty column sums to zero");

    near(d.RATE("v", "t"), 1.9, "RATE spans the whole selection", 1e-12);
    near(d.IRATE("v", "t"), 5.5, "IRATE uses only the last two rows", 1e-12);
    ok(d.RATE("v", "t") !== d.IRATE("v", "t"),
       "the two rates differ when the last interval is not the average");
    ok(Number.isNaN(new DataFrame({ a: Float64Array.from([1]), b: Float64Array.from([1]) })
        .RATE("a", "b")), "one row cannot give a rate");
}

S("grouped bitwise folds and the correlation matrix");
mark("GROUP_BIT_AND", "GROUP_BIT_OR", "GROUP_BIT_XOR", "CORR_MATRIX");
{
    const k = ["a", "b", "a", "c", "b", "a"];
    const v = Int32Array.from([12, 10, 6, 15, 3, 9]);
    const d = new DataFrame({ k, v });

    /* group a is {12,6,9}, b is {10,3}, c is {15} -- folded by hand so the test
       does not just restate the implementation */
    elemEq(d.GROUP_BIT_AND("k", "v").values, [12 & 6 & 9, 10 & 3, 15], "GROUP_BIT_AND");
    elemEq(d.GROUP_BIT_OR("k", "v").values,  [12 | 6 | 9, 10 | 3, 15], "GROUP_BIT_OR");
    elemEq(d.GROUP_BIT_XOR("k", "v").values, [12 ^ 6 ^ 9, 10 ^ 3, 15], "GROUP_BIT_XOR");
    eq(d.GROUP_BIT_AND("k", "v").keys.join(","), d.GROUP_BY_SUM("k", "v").keys.join(","),
       "the keys are element-for-element GROUP_BY_SUM's");

    /* the whole group folded is the scalar fold, which ties the two families */
    const one = new DataFrame({ g: Int32Array.from([0, 0, 0]), w: Int32Array.from([12, 6, 9]) });
    eq(one.GROUP_BIT_AND("g", "w").values[0], one.BITWISE_AND("w"),
       "one group is exactly the scalar BITWISE_AND");
    eq(one.GROUP_BIT_XOR("g", "w").values[0], one.BITWISE_XOR("w"),
       "and likewise for XOR");

    throwsLike(() => new DataFrame({ k, f: Float64Array.from([1,2,3,4,5,6]) })
               .GROUP_BIT_AND("k", "f"), "integer columns",
               "a float value column is refused, naming the dtype");

    /* y is 2x exactly and z is its reverse, so the matrix is +/-1 by construction */
    const m = new DataFrame({
        x: Float64Array.from([1, 2, 3, 4, 5]),
        y: Float64Array.from([2, 4, 6, 8, 10]),
        z: Float64Array.from([5, 4, 3, 2, 1]),
    });
    const cm = m.CORR_MATRIX(["x", "y", "z"]);
    eq(cm.n, 3, "CORR_MATRIX reports its size");
    eq(cm.columns.join(","), "x,y,z", "and echoes the columns in order");
    eq(cm.matrix.length, 9, "the matrix is n*n row-major");
    for (let i = 0; i < 3; i++)
        near(cm.matrix[i * 3 + i], 1, "the diagonal is 1 at " + i, 1e-12);
    for (let i = 0; i < 3; i++)
        for (let j = 0; j < 3; j++)
            near(cm.matrix[i * 3 + j], cm.matrix[j * 3 + i],
                 "symmetric at " + i + "," + j, 1e-12);
    near(cm.matrix[0 * 3 + 1], 1, "x and y are perfectly correlated", 1e-12);
    near(cm.matrix[0 * 3 + 2], -1, "x and z perfectly anti-correlated", 1e-12);
    /* no cell may disagree with CORR itself */
    /* EXACT, not a tolerance: a tolerance hid a ULP disagreement caused by an
       inline sqrt(a*b) where CORR uses sqrt(a)*sqrt(b) */
    eq(cm.matrix[0 * 3 + 1], m.CORR("x", "y"), "cell [x][y] IS CORR(x,y), bit for bit");
    eq(cm.matrix[1 * 3 + 2], m.CORR("y", "z"), "cell [y][z] IS CORR(y,z), bit for bit");
    throwsLike(() => m.CORR_MATRIX("x"), "must be an array",
               "a single name is refused; it takes a list");
}

S("string fold and grouped collection variants");
mark("GROUP_CONCAT", "GROUP_ARRAY_SORTED", "GROUP_ARRAY_LAST", "GROUP_ARRAY_SAMPLE");
{
    const k = ["a", "b", "a", "c", "b", "a"];
    const v = Float64Array.from([3, 1, 4, 1, 5, 9]);
    const d = new DataFrame({ k, v });

    eq(d.GROUP_CONCAT("k"), "a,b,a,c,b,a", "GROUP_CONCAT joins in ROW order, not group order");
    eq(d.GROUP_CONCAT("k", " | "), "a | b | a | c | b | a", "a separator is honoured");
    eq(d.GROUP_CONCAT("v", "-"), "3-1-4-1-5-9", "a numeric column joins its numbers");
    eq(d.GROUP_CONCAT("k", ",", Uint8Array.from([1,0,1,0,0,1])), "a,a,a",
       "a mask selects which rows are joined");
    eq(new DataFrame({ e: new Float64Array(0) }).GROUP_CONCAT("e"), "",
       "an empty column joins to the empty string");
    eq(d.GROUP_CONCAT("k", ",", new Uint8Array(6)), "",
       "a mask selecting nothing joins to the empty string, with no stray separator");

    /* the sorted form must be a permutation of the plain one, group for group */
    /* A NaN in the group must not stop the sort. The insertion sort this
       replaced compared with `>`, which is false against NaN, so NaN acted as a
       barrier and the group came back in ROW order -- named SORTED, unsorted. */
    {
        const nanG = new DataFrame({ k: ["a","a","a","a","a"],
                                     v: Float64Array.from([3, NaN, 1, NaN, 2]) });
        const got = Array.from(nanG.GROUP_ARRAY_SORTED("k", "v").values[0]);
        const ref = Array.from(nanG.SORT("v"));
        eq(got.length, ref.length, "a group with NaN keeps every row");
        for (let i = 0; i < ref.length; i++)
            ok((Number.isNaN(got[i]) && Number.isNaN(ref[i])) || got[i] === ref[i],
               "GROUP_ARRAY_SORTED matches SORT at " + i,
               got[i] + " vs " + ref[i]);
    }

    const plain = d.GROUP_ARRAY("k", "v"), sorted = d.GROUP_ARRAY_SORTED("k", "v");
    eq(sorted.keys.join(","), plain.keys.join(","), "SORTED keeps GROUP_ARRAY's keys");
    for (let g = 0; g < plain.keys.length; g++) {
        const a = Array.from(plain.values[g]).sort((x, y) => x - y);
        elemEq(sorted.values[g], a, "group " + plain.keys[g] + " is sorted ascending");
    }

    const last = d.GROUP_ARRAY_LAST("k", "v", 2);
    elemEq(last.values[0], [4, 9], "LAST keeps the final two rows of group a");
    elemEq(last.values[2], [1], "a group with fewer than k rows keeps all of them");
    for (let g = 0; g < last.keys.length; g++)
        ok(last.values[g].length <= 2, "no group exceeds k");

    const samp = d.GROUP_ARRAY_SAMPLE("k", "v", 1);
    for (let g = 0; g < samp.keys.length; g++)
        eq(samp.values[g].length, 1, "SAMPLE takes exactly k where the group has them");
    elemEq(d.GROUP_ARRAY_SAMPLE("k", "v", 1).values[0],
           d.GROUP_ARRAY_SAMPLE("k", "v", 1).values[0],
           "SAMPLE is deterministic: two calls agree");

    throwsLike(() => d.GROUP_ARRAY_LAST("k", "v", 0), "[1, 65536]", "k of zero is refused");
    throwsLike(() => d.GROUP_ARRAY_LAST("k", "v", 2.5), "[1, 65536]",
               "a fractional k is refused rather than truncated");
    throwsLike(() => new DataFrame({ k, s: ["x","y","x","z","y","x"] })
               .GROUP_ARRAY_SORTED("k", "s"), "only numeric",
               "a string VALUE column is refused, naming the type");
}

S("weighted frequency, heavy hitter, weighted quantile, group intersection");
mark("TOP_K_WEIGHTED", "APPROX_TOP_SUM", "ANY_HEAVY",
     "QUANTILE_EXACT_WEIGHTED", "GROUP_ARRAY_INTERSECT");
{
    /* value 1 is rare but heavy; 3 is frequent but light. The weighted and
       unweighted answers must therefore DISAGREE, which is the whole point. */
    const v = Float64Array.from([1, 2, 2, 3, 3, 3]);
    const w = Float64Array.from([10, 1, 1, 1, 1, 1]);
    const d = new DataFrame({ v, w });

    const tw = d.TOP_K_WEIGHTED("v", "w", 2);
    eq(tw.keys.join(","), "1,3", "TOP_K_WEIGHTED ranks by summed weight");
    elemEq(tw.values, [10, 3], "and reports those weights");
    eq(d.TOP_K("v", 2).keys.join(","), "3,2", "unweighted TOP_K ranks by COUNT");
    ok(tw.keys.join(",") !== d.TOP_K("v", 2).keys.join(","),
       "the two rankings differ, so weight is really being used");

    const ts = d.APPROX_TOP_SUM("v", "w", 2);
    eq(ts.keys.join(","), tw.keys.join(","), "APPROX_TOP_SUM is the same answer");
    elemEq(ts.values, tw.values, "and the same weights");

    /* strictly more than half: 10 of 15 qualifies, 3 of 6 does not */
    eq(d.ANY_HEAVY("v", "w"), 1, "ANY_HEAVY finds the majority-weight value");
    eq(d.ANY_HEAVY("v"), undefined,
       "unweighted, no value holds STRICTLY more than half, so undefined");
    eq(new DataFrame({ e: new Float64Array(0) }).ANY_HEAVY("e"), undefined,
       "ANY_HEAVY of an empty column");

    /* weight 10 on the smallest value drags the median down to it */
    eq(d.QUANTILE_EXACT_WEIGHTED("v", "w", 0.5), 1, "the weighted median is 1");
    eq(d.MEDIAN("v"), 2.5, "the unweighted median is 2.5");
    eq(d.QUANTILE_EXACT_WEIGHTED("v", "w", 0), 1, "q=0 is the smallest value");
    eq(d.QUANTILE_EXACT_WEIGHTED("v", "w", 1), 3, "q=1 is the largest");
    throwsLike(() => d.QUANTILE_EXACT_WEIGHTED("v", "w", 2), "[0, 1]",
               "an out-of-range q is refused");
    eq(new DataFrame({ a: new Float64Array(0), b: new Float64Array(0) })
       .QUANTILE_EXACT_WEIGHTED("a", "b", 0.5), undefined, "empty gives undefined");

    /* a={1,2} b={2,3} c={2} -> only 2 is in every group */
    const g = new DataFrame({ k: ["a","a","b","b","c"], x: Float64Array.from([1,2,2,3,2]) });
    elemEq(g.GROUP_ARRAY_INTERSECT("k", "x"), [2], "only the shared value survives");
    elemEq(new DataFrame({ k: ["a","a","b"], x: Float64Array.from([1,2,3]) })
           .GROUP_ARRAY_INTERSECT("k", "x"), [], "disjoint groups intersect to nothing");
    elemEq(new DataFrame({ k: ["a","b"], x: Float64Array.from([7,7]) })
           .GROUP_ARRAY_INTERSECT("k", "x"), [7], "a value in every group survives");
    /* a repeat inside ONE group must not count as two groups */
    elemEq(new DataFrame({ k: ["a","a"], x: Float64Array.from([5,5]) })
           .GROUP_ARRAY_INTERSECT("k", "x"), [5],
           "a repeat within one group is still one group");
}

S("bounding ratio, time-decayed average, positional insert, bitmap, weighted t-digest, ranges");
mark("BOUNDING_RATIO", "EXPONENTIAL_TIME_DECAYED_AVG", "GROUP_ARRAY_INSERT_AT",
     "GROUP_BITMAP", "QUANTILE_TDIGEST_WEIGHTED", "RANGE_AGG",
     "RANGE_INTERSECT_AGG");
{
    /* Rows are deliberately NOT in x order, so a method that reads the first
       and last ROW gets a different answer from one that reads the smallest and
       largest x. That is exactly what separates BOUNDING_RATIO from RATE. */
    const d = new DataFrame({
        x: Float64Array.from([3, 1, 2, 5, 4]),
        y: Float64Array.from([30, 10, 20, 50, 41]),
        t: Float64Array.from([0, 1, 2, 3, 4]),
    });
    eq(d.BOUNDING_RATIO("x", "y"), 10, "BOUNDING_RATIO uses the extreme x values");
    eq(d.RATE("y", "x"), 11, "RATE uses the first and last ROWS, and disagrees");
    ok(d.BOUNDING_RATIO("x", "y") !== d.RATE("y", "x"),
       "so the two are not synonyms");
    eq(new DataFrame({ a: Float64Array.from([2]), b: Float64Array.from([9]) })
       .BOUNDING_RATIO("a", "b"), NaN, "one point has no slope");
    eq(new DataFrame({ a: Float64Array.from([2, 2]), b: Float64Array.from([1, 9]) })
       .BOUNDING_RATIO("a", "b"), NaN, "a vertical line has no slope");
    eq(new DataFrame({ a: new Float64Array(0), b: new Float64Array(0) })
       .BOUNDING_RATIO("a", "b"), NaN, "empty has no slope");

    /* A huge tau makes every weight 1, so the decayed mean MUST collapse onto
       the plain mean -- that limit is the oracle, not a hand-computed constant. */
    near(d.EXPONENTIAL_TIME_DECAYED_AVG("y", "t", 1e12), d.MEAN("y"),
         "an effectively infinite tau is the plain mean", 1e-9);
    ok(d.EXPONENTIAL_TIME_DECAYED_AVG("y", "t", 0.5) > d.MEAN("y"),
       "a short tau leans on the LATEST rows, which here are the larger y");
    throwsLike(() => d.EXPONENTIAL_TIME_DECAYED_AVG("y", "t", 0), "positive",
               "tau must be positive");
    throwsLike(() => d.EXPONENTIAL_TIME_DECAYED_AVG("y", "t", -1), "positive",
               "a negative tau is refused, not squared");
    eq(new DataFrame({ a: new Float64Array(0), b: new Float64Array(0) })
       .EXPONENTIAL_TIME_DECAYED_AVG("a", "b", 1), undefined, "empty gives undefined");

    const ins = new DataFrame({
        v: Float64Array.from([30, 10, 20, 50, 40]),
        p: Int32Array.from([2, 0, 1, 4, 3]),
    });
    elemEq(ins.GROUP_ARRAY_INSERT_AT("v", "p", 6, -1), [10, 20, 30, 40, 50, -1],
           "each value lands at its own position, the gap keeps the fill");
    elemEq(ins.GROUP_ARRAY_INSERT_AT("v", "p", 3, -1), [10, 20, 30],
           "positions past the end are dropped, not grown into");
    elemEq(ins.GROUP_ARRAY_INSERT_AT("v", "p", 0, -1), [],
           "size 0 is legal and empty");
    elemEq(new DataFrame({ v: Float64Array.from([7, 8]), p: Int32Array.from([1, 1]) })
           .GROUP_ARRAY_INSERT_AT("v", "p", 2, 0), [0, 8],
           "a later row overwrites an earlier one at the same position");
    throwsLike(() => ins.GROUP_ARRAY_INSERT_AT("v", "p", -1, 0), "size must be",
               "a negative size is refused");
    throwsLike(() => ins.GROUP_ARRAY_INSERT_AT("v", "p", 1.5, 0), "size must be",
               "a fractional size is refused rather than truncated");

    /* The bitmap and the hash must agree on every integer column, or one of
       them is wrong; that agreement is the oracle. */
    for (const [tag, T] of INTS) {
        if (tag === "i8" || tag === "i16" || tag === "i32") continue;
        const c = new T([5, 5, 7, 9, 9, 0]);
        eq(new DataFrame({ c }).GROUP_BITMAP("c"),
           new DataFrame({ c }).N_UNIQUE("c"),
           "GROUP_BITMAP agrees with N_UNIQUE on " + tag);
    }
    eq(new DataFrame({ c: new Int32Array(0) }).GROUP_BITMAP("c"), 0,
       "an empty column has no distinct values");
    throwsLike(() => new DataFrame({ c: Int32Array.from([-1]) }).GROUP_BITMAP("c"),
               "negative", "a negative value is refused: a bitmap indexes by value");
    throwsLike(() => new DataFrame({ c: Float64Array.from([1]) }).GROUP_BITMAP("c"),
               "integer columns", "a float column is refused by name");
    throwsLike(() => new DataFrame({ c: Int32Array.from([1 << 30]) }).GROUP_BITMAP("c"),
               "N_UNIQUE", "a value past the bitmap range names the alternative");

    /* Weighting the smallest value heavily must drag the approximate quantile
       the same WAY the exact one moves; the digest is not exact, so the
       assertion is the direction and a tolerance, never equality. */
    const wv = new DataFrame({
        v: Float64Array.from([1, 2, 2, 3, 3, 3]),
        w: Float64Array.from([10, 1, 1, 1, 1, 1]),
    });
    near(wv.QUANTILE_TDIGEST_WEIGHTED("v", "w", 0.5),
         wv.QUANTILE_EXACT_WEIGHTED("v", "w", 0.5),
         "the weighted digest tracks the exact weighted median", 0.5);
    ok(wv.QUANTILE_TDIGEST_WEIGHTED("v", "w", 0.5) < wv.MEDIAN("v"),
       "and it is dragged below the unweighted median");
    throwsLike(() => wv.QUANTILE_TDIGEST_WEIGHTED("v", "w", 2), "[0, 1]",
               "an out-of-range q is refused");
    eq(new DataFrame({ a: new Float64Array(0), b: new Float64Array(0) })
       .QUANTILE_TDIGEST_WEIGHTED("a", "b", 0.5), undefined, "empty gives undefined");
    eq(new DataFrame({ a: Float64Array.from([1, 2]), b: Float64Array.from([0, 0]) })
       .QUANTILE_TDIGEST_WEIGHTED("a", "b", 0.5), undefined,
       "all-zero weights select nothing");

    /* [1,4) [2,6) [8,9) [3,5) [20,25): the first, second and fourth overlap into
       [1,6); the others stand alone. Order in, order out is sorted. */
    const rg = new DataFrame({
        lo: Float64Array.from([1, 2, 8, 3, 20]),
        hi: Float64Array.from([4, 6, 9, 5, 25]),
    });
    const u = rg.RANGE_AGG("lo", "hi");
    elemEq(u.starts, [1, 8, 20], "RANGE_AGG merges the overlapping run");
    elemEq(u.ends, [6, 9, 25], "and reports each union's end");
    eq(rg.RANGE_INTERSECT_AGG("lo", "hi"), undefined,
       "disjoint ranges have no common interval");

    const tj = new DataFrame({ lo: Float64Array.from([1, 2]), hi: Float64Array.from([2, 3]) });
    const tu = tj.RANGE_AGG("lo", "hi");
    elemEq(tu.starts, [1], "touching half-open ranges join: [1,2) and [2,3) leave no gap");
    elemEq(tu.ends, [3], "and cover [1,3)");

    const ov = new DataFrame({ lo: Float64Array.from([1, 2, 3]), hi: Float64Array.from([10, 9, 8]) });
    const iv = ov.RANGE_INTERSECT_AGG("lo", "hi");
    eq(iv.start, 3, "the intersection starts at the greatest start");
    eq(iv.end, 8, "and ends at the least end");
    const single = ov.RANGE_AGG("lo", "hi");
    elemEq(single.starts, [1], "nested ranges collapse to the outermost");
    elemEq(single.ends, [10], "which is [1,10)");

    /* An inverted or empty range covers nothing and must not survive either. */
    const bad = new DataFrame({ lo: Float64Array.from([5, 1]), hi: Float64Array.from([5, 3]) });
    const bu = bad.RANGE_AGG("lo", "hi");
    elemEq(bu.starts, [1], "an empty range [5,5) is dropped");
    elemEq(bu.ends, [3], "leaving only the real one");
    const emptyR = new DataFrame({ lo: new Float64Array(0), hi: new Float64Array(0) });
    eq(emptyR.RANGE_AGG("lo", "hi").starts.length, 0, "no ranges in, none out");
    eq(emptyR.RANGE_INTERSECT_AGG("lo", "hi"), undefined, "and no intersection");
}

ok(hooksFired >= 12, "every attack injected code ran at least once",
   "total hook invocations " + hooksFired);

/* =============================== dispersion, rank conventions, change (G8)
   Every expected value below comes from the definition or a cited outside
   source, never read back from the engine -- that freezes today's bugs. */
S("rolling dispersion");
{
    mark("ROLLING_VAR", "ROLLING_STD");
    const n = 40, x = new Float64Array(n);
    for (let i = 0; i < n; i++) x[i] = ((i * 37) % 17) - 8 + i * 0.25;
    const df = new DataFrame({ x });

    /* reference: two-pass sample variance over each closed window */
    const refVar = (w) => {
        const out = [];
        for (let i = 0; i < n; i++) {
            if (i + 1 < w) { out.push(NaN); continue; }
            let s = 0, c = 0;
            for (let j = i + 1 - w; j <= i; j++) { s += x[j]; c++; }
            const m = s / c;
            let q = 0;
            for (let j = i + 1 - w; j <= i; j++) q += (x[j] - m) * (x[j] - m);
            out.push(q / (c - 1));
        }
        return out;
    };
    for (const w of [2, 3, 8, 9, 17]) {
        const got = df.ROLLING_VAR("x", w), want = refVar(w);
        let worst = 0;
        for (let i = 0; i < n; i++) {
            if (Number.isNaN(want[i])) { ok(Number.isNaN(got[i]), "w=" + w + " row " + i + " is NaN before the window fills"); continue; }
            worst = Math.max(worst, Math.abs(got[i] - want[i]) / Math.max(1, Math.abs(want[i])));
        }
        ok(worst < 1e-12, "ROLLING_VAR w=" + w + " matches the two-pass definition",
           "worst relative error " + worst);
    }
    const sd = df.ROLLING_STD("x", 8), vr = df.ROLLING_VAR("x", 8);
    let sdOk = true;
    for (let i = 7; i < n; i++) if (Math.abs(sd[i] - Math.sqrt(vr[i])) > 1e-12) sdOk = false;
    ok(sdOk, "ROLLING_STD is exactly sqrt(ROLLING_VAR)");

    /* w=1 has one row per window: no sample variance exists, so NaN, not 0. */
    ok(df.ROLLING_VAR("x", 1).every((v) => Number.isNaN(v)),
       "a window of 1 has no sample variance and is all NaN");
    /* a masked window can drop below two contributors even when w is large. */
    const m1 = new Uint8Array(n);
    m1[0] = 1;
    ok(Number.isNaN(df.ROLLING_VAR("x", 4, m1)[3]),
       "one selected row in the window yields NaN, not 0");
    /* NaN PROPAGATES here, like VARIANCE and ROLLING_MEAN and unlike
       ROLLING_MIN/MAX. Ignoring it would report the variance of a SUBSET of the
       window with nothing to say so -- a plausible wrong answer. */
    {
        const h = new DataFrame({ v: Float64Array.from([1, 2, NaN, 4, 5, 6, 7, 8]) });
        const rv = h.ROLLING_VAR("v", 3), rm = h.ROLLING_MEAN("v", 3);
        for (const i of [2, 3, 4]) {
            ok(Number.isNaN(rv[i]), "row " + i + " touches the NaN so ROLLING_VAR is NaN");
            ok(Number.isNaN(rm[i]), "and ROLLING_MEAN agrees at row " + i);
        }
        ok(Number.isNaN(h.VARIANCE("v")), "as does the non-rolling VARIANCE");
        ok(!Number.isNaN(rv[7]), "a window clear of the NaN still answers");
        eq(rv[7], 1, "and answers correctly");
        /* min/max are the family that DOES ignore NaN -- the contrast is the point */
        ok(!Number.isNaN(h.ROLLING_MIN("v", 3)[3]), "ROLLING_MIN still ignores NaN");
        /* an infinity already reached NaN through Inf-Inf; keep it pinned */
        const inf = new DataFrame({ v: Float64Array.from([1, 2, Infinity, 4, 5, 6]) });
        ok(Number.isNaN(inf.ROLLING_VAR("v", 3)[3]), "an infinity in the window is NaN");
        ok(Number.isNaN(inf.VARIANCE("v")), "matching VARIANCE, which is also NaN");
    }
    /* Above span*w = 1e8 a centred block decomposition replaces the O(span*w)
       two-pass: 21.9 s -> 1.4 ms at span=200k, w=100k. It is NOT exact, which
       is why it is gated -- so both sides of the gate are checked. */
    {
        const bn = 200000, bx = new Float64Array(bn);
        for (let i = 0; i < bn; i++) bx[i] = Math.sin(i * 0.37) * 1000 + i * 0.5;
        const bf = new DataFrame({ x: bx });
        for (const w of [1000, 100000]) {
            const t0 = Date.now();
            const r = bf.ROLLING_VAR("x", w);
            const took = Date.now() - t0;
            eq(r.length, bn, "w=" + w + " keeps the column length");
            ok(took < 2000, "w=" + w + " is bounded (took " + took + " ms)");
            /* an independent two-pass over the LAST window only */
            const i = bn - 1;
            let sum = 0;
            for (let j = i + 1 - w; j <= i; j++) sum += bx[j];
            const mu = sum / w;
            let q = 0;
            for (let j = i + 1 - w; j <= i; j++) q += (bx[j] - mu) * (bx[j] - mu);
            const want = q / (w - 1);
            ok(Math.abs(r[i] - want) / want < 1e-10,
               "w=" + w + " the fast path still matches a two-pass reference",
               "got " + r[i] + " want " + want);
            ok(Number.isNaN(r[w - 2]) && !Number.isNaN(r[w - 1]),
               "w=" + w + " fills at exactly w-1, as the exact path does");
        }
        /* below the gate the exact path must still run: span*w = 4e6 here */
        const sn = 4000, sx = new Float64Array(sn);
        for (let i = 0; i < sn; i++) sx[i] = Math.sin(i * 0.7) * 3;
        const sf = new DataFrame({ x: sx });
        const rs = sf.ROLLING_VAR("x", 1024);
        ok(!Number.isNaN(rs[1023]) && Number.isNaN(rs[1022]),
           "below the gate the exact path still fills at w-1");
    }
    throwsLike(() => df.ROLLING_VAR("x", 0), "positive integer",
               "ROLLING_VAR refuses a zero window");
    throwsLike(() => df.ROLLING_STD("x", 2.5), "positive integer",
               "ROLLING_STD refuses a fractional window");
}

S("pct change / zscore");
{
    mark("PCT_CHANGE", "ZSCORE");
    const v = Float64Array.from([4, 5, 10, 10, 0, -2, 8]);
    const df = new DataFrame({ v });

    const pc = df.PCT_CHANGE("v");
    ok(Number.isNaN(pc[0]), "the first row has no predecessor");
    eq(pc[1], 0.25, "5 from 4 is +25%");
    eq(pc[2], 1, "10 from 5 is +100%");
    eq(pc[3], 0, "no change is 0, not NaN");
    eq(pc[4], -1, "0 from 10 is -100%");
    ok(pc[5] === -Infinity, "a change away from 0 is -Infinity, honestly");
    const pc2 = df.PCT_CHANGE("v", 2);
    ok(Number.isNaN(pc2[0]) && Number.isNaN(pc2[1]), "periods=2 skips two rows");
    eq(pc2[2], 1.5, "10 from 4 over two periods");
    throwsLike(() => df.PCT_CHANGE("v", 0), "positive integer",
               "PCT_CHANGE refuses zero periods");

    /* reference: sample stddev, so it composes with STDDEV not STDDEV_POP */
    let s = 0;
    for (let i = 0; i < v.length; i++) s += v[i];
    const mu = s / v.length;
    let q = 0;
    for (let i = 0; i < v.length; i++) q += (v[i] - mu) * (v[i] - mu);
    const sd = Math.sqrt(q / (v.length - 1));
    const z = df.ZSCORE("v");
    let zw = 0;
    for (let i = 0; i < v.length; i++)
        zw = Math.max(zw, Math.abs(z[i] - (v[i] - mu) / sd));
    ok(zw < 1e-12, "ZSCORE matches (x-mean)/sample stddev", "worst " + zw);
    let zs = 0;
    for (let i = 0; i < v.length; i++) zs += z[i];
    ok(Math.abs(zs) < 1e-12, "a z-scored column sums to zero", "sum " + zs);
    /* a constant column has no spread: NaN, never a column of zeros. */
    const flat = new DataFrame({ c: Float64Array.from([3, 3, 3, 3]) });
    ok(flat.ZSCORE("c").every((t) => Number.isNaN(t)),
       "a constant column z-scores to NaN, not 0");
}

S("rank conventions");
{
    mark("DENSE_RANK", "PERCENT_RANK", "NTILE", "RANK_CORR");
    /* ties at 20 and at 40, so the three conventions must visibly disagree. */
    const k = Float64Array.from([10, 20, 20, 30, 40, 40, 40]);
    const df = new DataFrame({ k });
    elemEq(df.RANK("k"), [1, 2.5, 2.5, 4, 6, 6, 6], "RANK averages a tie");
    elemEq(df.DENSE_RANK("k"), [1, 2, 2, 3, 4, 4, 4], "DENSE_RANK leaves no gap");
    /* SQL: (minrank-1)/(m-1), so first is 0 and last is 1 */
    const pr = df.PERCENT_RANK("k"), wantPr = [0, 1 / 6, 1 / 6, 3 / 6, 4 / 6, 4 / 6, 4 / 6];
    let pw = 0;
    for (let i = 0; i < pr.length; i++) pw = Math.max(pw, Math.abs(pr[i] - wantPr[i]));
    ok(pw < 1e-15, "PERCENT_RANK is (minrank-1)/(m-1)", "worst " + pw);
    eq(df.PERCENT_RANK("k")[0], 0, "the smallest row is exactly 0");
    const one = new DataFrame({ k: Float64Array.from([7]) });
    eq(one.PERCENT_RANK("k")[0], 0, "a single row is 0, not a division by zero");

    /* NTILE: 7 rows into 3 tiles is 3,2,2 -- the first m%k tiles take the extra */
    elemEq(df.NTILE("k", 3), [1, 1, 1, 2, 2, 3, 3], "NTILE gives sizes 3,2,2");
    elemEq(df.NTILE("k", 1), [1, 1, 1, 1, 1, 1, 1], "one tile takes everything");
    const t7 = df.NTILE("k", 7);
    elemEq(t7, [1, 2, 3, 4, 5, 6, 7], "as many tiles as rows is one row each");
    /* more tiles than rows: the trailing tiles are empty, none is out of range */
    /* NOT `every(t => t >= 1 && t <= 9)`: that predicate is also true of
       [1,1,1,1,1,1,1]. 7 rows into 9 tiles is one row each, tiles 8 and 9 empty. */
    elemEq(df.NTILE("k", 9), [1, 2, 3, 4, 5, 6, 7],
           "more tiles than rows gives one row each, not a clamp");
    throwsLike(() => df.NTILE("k", 0), "positive integer", "NTILE refuses zero buckets");

    /* NaN is ranked by no convention: it stays NaN in all four. */
    const withNaN = new DataFrame({ k: Float64Array.from([3, NaN, 1]) });
    for (const m of ["RANK", "DENSE_RANK", "PERCENT_RANK"])
        ok(Number.isNaN(withNaN[m]("k")[1]), m + " leaves a NaN row unranked");
    ok(Number.isNaN(withNaN.NTILE("k", 2)[1]), "NTILE leaves a NaN row unranked");

    /* Spearman: exactly 1 on any strictly increasing transform, which is the
       property that distinguishes it from Pearson. */
    const mono = new DataFrame({
        a: Float64Array.from([1, 2, 3, 4, 5, 6]),
        b: Float64Array.from([1, 8, 27, 64, 125, 216]),
        c: Float64Array.from([6, 5, 4, 3, 2, 1]),
    });
    ok(Math.abs(mono.RANK_CORR("a", "b") - 1) < 1e-12,
       "Spearman is 1 on a cube, where Pearson is not", "got " + mono.RANK_CORR("a", "b"));
    ok(mono.CORR("a", "b") < 0.98, "and Pearson on the same pair is not 1",
       "Pearson " + mono.CORR("a", "b"));
    ok(Math.abs(mono.RANK_CORR("a", "c") + 1) < 1e-12, "reversed is -1");
    eq(mono.RANK_CORR("a", "a"), 1, "a column against itself is 1");
    /* pairwise selection: a NaN in either column drops the ROW from both
       rankings, so the answer must still be a perfect +1 here. */
    const holed = new DataFrame({
        a: Float64Array.from([1, NaN, 3, 4, 5]),
        b: Float64Array.from([2, 9, 6, NaN, 10]),
    });
    ok(Math.abs(holed.RANK_CORR("a", "b") - 1) < 1e-12,
       "a NaN in either column drops the row from BOTH rankings",
       "got " + holed.RANK_CORR("a", "b"));

    /* Ties are the ONLY case separating the two formulas: everything above is
       tie-free and passes against the naive 1-6*sum(d^2)/(n(n^2-1)) shortcut
       too. Expected = scipy 1.18.0 spearmanr; 4th column = the shortcut. */
    for (const [name, xs, ys, scipyRho, shortcut] of [
        ["heavy ties both", [1, 1, 2, 2, 3, 3, 4, 4], [2, 2, 2, 5, 5, 9, 9, 9],
         0.9036961141150639, 0.910714],
        ["ties in x only", [1, 1, 1, 2, 3, 4, 5, 5], [3, 1, 4, 1, 5, 9, 2, 6],
         0.44457998712659669, 0.464286],
        ["perfect with ties", [1, 1, 2, 2, 3, 3], [4, 4, 7, 7, 9, 9], 1, 1],
        ["anti with ties", [1, 1, 2, 2, 3, 3], [9, 9, 7, 7, 4, 4], -1, -0.828571],
    ]) {
        const tf = new DataFrame({ x: Float64Array.from(xs), y: Float64Array.from(ys) });
        const got = tf.RANK_CORR("x", "y");
        ulpNear(got, scipyRho, 4, "RANK_CORR " + name + " matches scipy 1.18.0");
        if (scipyRho !== shortcut)
            ok(Math.abs(got - shortcut) > 1e-4,
               "and is NOT the tie-broken shortcut for " + name,
               "got " + got + ", shortcut would be " + shortcut);
    }
}

S("time-decayed family");
{
    mark("EXPONENTIAL_TIME_DECAYED_SUM", "EXPONENTIAL_TIME_DECAYED_COUNT",
         "EXPONENTIAL_TIME_DECAYED_MAX");
    /* weights are exp(-(tMax - t)/tau), so the reference is computed here from
       that definition rather than read back from the engine */
    const v = Float64Array.from([1, 2, 3, 4]);
    const ts = Float64Array.from([0, 10, 20, 30]);
    const df = new DataFrame({ v, ts });
    const tau = 10, tmax = 30;
    let num = 0, den = 0, best = -Infinity;
    for (let i = 0; i < 4; i++) {
        const w = Math.exp((ts[i] - tmax) / tau);
        num += v[i] * w; den += w; best = Math.max(best, v[i] * w);
    }
    near(df.EXPONENTIAL_TIME_DECAYED_SUM("v", "ts", tau), num, "SUM is the weighted total");
    near(df.EXPONENTIAL_TIME_DECAYED_COUNT("v", "ts", tau), den, "COUNT is the weighted row count");
    near(df.EXPONENTIAL_TIME_DECAYED_MAX("v", "ts", tau), best, "MAX is the largest weighted value");
    near(df.EXPONENTIAL_TIME_DECAYED_AVG("v", "ts", tau), num / den, "AVG is SUM/COUNT");
    /* the identity is the point of the family: it must hold exactly, not nearly */
    near(df.EXPONENTIAL_TIME_DECAYED_SUM("v", "ts", tau) /
         df.EXPONENTIAL_TIME_DECAYED_COUNT("v", "ts", tau),
         df.EXPONENTIAL_TIME_DECAYED_AVG("v", "ts", tau), "SUM/COUNT reproduces AVG");
    /* COUNT ignores the VALUE column -- that is what makes it a count */
    const df2 = new DataFrame({ v: Float64Array.from([9, 9, 9, 9]), ts });
    near(df2.EXPONENTIAL_TIME_DECAYED_COUNT("v", "ts", tau),
         df.EXPONENTIAL_TIME_DECAYED_COUNT("v", "ts", tau),
         "COUNT is unchanged by different values");
    /* the newest row has weight exactly 1, so COUNT is never below 1 */
    ok(df.EXPONENTIAL_TIME_DECAYED_COUNT("v", "ts", tau) >= 1,
       "the latest row weighs exactly 1, so COUNT >= 1");
    /* every one refuses a non-positive tau, and names ITSELF when it does */
    for (const m of ["SUM", "COUNT", "MAX", "AVG"]) {
        throwsLike(() => df["EXPONENTIAL_TIME_DECAYED_" + m]("v", "ts", 0),
                   "EXPONENTIAL_TIME_DECAYED_" + m,
                   m + " names itself when refusing tau=0");
        throwsLike(() => df["EXPONENTIAL_TIME_DECAYED_" + m]("v", "ts", -1),
                   "must be positive", m + " refuses a negative tau");
    }
    /* nothing selected is undefined, not 0 */
    const none = new Uint8Array(4);
    for (const m of ["SUM", "COUNT", "MAX", "AVG"])
        eq(df["EXPONENTIAL_TIME_DECAYED_" + m]("v", "ts", tau, none), undefined,
           m + " with nothing selected is undefined");
}

S("group array moving");
{
    mark("GROUP_ARRAY_MOVING_SUM", "GROUP_ARRAY_MOVING_AVG");
    /* keys are integer codes, values deliberately out of order within a group
       so a wrong row order shows up rather than cancelling */
    const k = Int32Array.from([0, 1, 0, 2, 1, 0, 2, 1]);
    const v = Float64Array.from([5, 1, -2, 7, 4, 10, 0, 3]);
    const df = new DataFrame({ k, v });

    /* independent reference: gather in ROW order, then fold */
    const slices = [[], [], []];
    for (let i = 0; i < k.length; i++) slices[k[i]].push(v[i]);
    const refMove = (a, w, avg) => a.map((_, i) => {
        const lo = w ? Math.max(0, i + 1 - w) : 0;
        let t = 0;
        for (let j = lo; j <= i; j++) t += a[j];
        return avg ? t / (i - lo + 1) : t;
    });

    const ms = df.GROUP_ARRAY_MOVING_SUM("k", "v");
    elemEq(ms.keys, [0, 1, 2], "moving sum reports the group keys");
    eq(ms.values.length, 3, "one array per group");
    for (let g = 0; g < 3; g++) {
        elemEq(ms.values[g], refMove(slices[g], 0, false),
               "group " + g + " expands: a running sum");
        elemEq(df.GROUP_ARRAY_MOVING_AVG("k", "v").values[g],
               refMove(slices[g], 0, true),
               "group " + g + " expands: a running mean");
        for (const w of [1, 2, 3]) {
            elemEq(df.GROUP_ARRAY_MOVING_SUM("k", "v", w).values[g],
                   refMove(slices[g], w, false),
                   "group " + g + " window " + w + " sum");
            elemEq(df.GROUP_ARRAY_MOVING_AVG("k", "v", w).values[g],
                   refMove(slices[g], w, true),
                   "group " + g + " window " + w + " mean");
        }
        /* the values are the same rows GROUP_ARRAY hands back, in the same order */
        elemEq(df.GROUP_ARRAY("k", "v").values[g], slices[g],
               "group " + g + " matches GROUP_ARRAY's own collection");
    }

    /* Two INDEPENDENT oracles that already ship: the last element of an
       expanding sum is that group's total, and of an expanding mean, its mean. */
    const gs = df.GROUP_BY_SUM("k", "v"), gm = df.GROUP_BY_MEAN("k", "v");
    const avg0 = df.GROUP_ARRAY_MOVING_AVG("k", "v");
    for (let g = 0; g < 3; g++) {
        const a = ms.values[g], b = avg0.values[g];
        near(a[a.length - 1], gs.values[g],
             "group " + g + " expanding sum ends at GROUP_BY_SUM");
        near(b[b.length - 1], gm.values[g],
             "group " + g + " expanding mean ends at GROUP_BY_MEAN");
    }

    /* a window at or past the group length IS the expanding form */
    for (const w of [3, 4, 100]) {
        for (let g = 0; g < 3; g++)
            elemEq(df.GROUP_ARRAY_MOVING_SUM("k", "v", w).values[g],
                   ms.values[g], "w=" + w + " >= group " + g + " length is expanding");
    }
    /* w = 1 is the identity */
    for (let g = 0; g < 3; g++) {
        elemEq(df.GROUP_ARRAY_MOVING_SUM("k", "v", 1).values[g], slices[g],
               "w=1 sum is the value itself");
        elemEq(df.GROUP_ARRAY_MOVING_AVG("k", "v", 1).values[g], slices[g],
               "w=1 mean is the value itself");
    }

    /* AVG divides by what CONTRIBUTED, not by w -- so it does NOT ramp up from
       a smaller first value the way ClickHouse's groupArrayMovingAvg does. */
    const a2 = df.GROUP_ARRAY_MOVING_AVG("k", "v", 2).values[0];
    eq(a2[0], slices[0][0], "the first element of a w=2 mean is the element, not half it");
    near(a2[1], (slices[0][0] + slices[0][1]) / 2, "and the second is a real mean of two");

    /* mask filters rows BEFORE grouping, as everywhere else in the module */
    const m = new Uint8Array(k.length);
    for (let i = 0; i < k.length; i++) m[i] = i % 2 ? 0 : 1;
    const masked = df.GROUP_ARRAY_MOVING_SUM("k", "v", 0 || undefined, m);
    const mslices = [[], [], []];
    for (let i = 0; i < k.length; i++) if (m[i]) mslices[k[i]].push(v[i]);
    for (let g = 0; g < 3; g++)
        elemEq(masked.values[g], refMove(mslices[g], 0, false),
               "masked group " + g + " sees only selected rows");

    /* an empty group is a zero-length array, never a hole */
    const none = new Uint8Array(k.length);
    const empty = df.GROUP_ARRAY_MOVING_SUM("k", "v", undefined, none);
    for (let g = 0; g < 3; g++)
        eq(empty.values[g].length, 0, "an all-masked group " + g + " is empty, not a hole");

    /* NaN propagates through an expanding sum and only through the windows that
       hold it -- the same asymmetry the two definitions have. */
    const nf = new DataFrame({
        k: Int32Array.from([0, 0, 0, 0]),
        v: Float64Array.from([1, NaN, 3, 4]),
    });
    const ne = nf.GROUP_ARRAY_MOVING_SUM("k", "v").values[0];
    eq(ne[0], 1, "before the NaN the expanding sum is finite");
    ok(Number.isNaN(ne[1]) && Number.isNaN(ne[2]) && Number.isNaN(ne[3]),
       "and every element after it is NaN");
    const nw = nf.GROUP_ARRAY_MOVING_SUM("k", "v", 2).values[0];
    eq(nw[3], 7, "a w=2 window clear of the NaN still answers");

    /* Above DFG_ROLL_SLIDE_MIN (256) a block decomposition replaces the
       re-sum. Everything above uses w <= 3, so without these the fast path
       ships unexercised -- and it is where the O(m^2) used to live. */
    for (const m of [255, 256, 257, 512, 1000]) {
        const kk = new Int32Array(m), vv = new Float64Array(m);
        for (let i = 0; i < m; i++) { kk[i] = 0; vv[i] = Math.sin(i * 0.37) * 1000 + i * 0.25; }
        const bf = new DataFrame({ k: kk, v: vv });
        for (const w of [255, 256, 257, 300]) {
            if (w >= m) continue;
            for (const avg of [false, true]) {
                const got = bf[avg ? "GROUP_ARRAY_MOVING_AVG" : "GROUP_ARRAY_MOVING_SUM"]("k", "v", w).values[0];
                eq(got.length, m, "m=" + m + " w=" + w + " keeps the group length");
                let worst = 0;
                for (let i = 0; i < m; i++) {
                    const lo = Math.max(0, i + 1 - w);
                    let t = 0;
                    for (let j = lo; j <= i; j++) t += vv[j];
                    const want = avg ? t / (i - lo + 1) : t;
                    worst = Math.max(worst, Math.abs(got[i] - want) / Math.max(1, Math.abs(want)));
                }
                ok(worst < 1e-12, "m=" + m + " w=" + w + (avg ? " AVG" : " SUM") +
                   " crosses the block gate and matches the reference",
                   "worst relative error " + worst);
            }
        }
    }
    /* the partial prefix must stay partial, not zero and not the first full window */
    {
        const m = 600, kk = new Int32Array(m), vv = new Float64Array(m).fill(1);
        const bf = new DataFrame({ k: kk, v: vv });
        const a1 = bf.GROUP_ARRAY_MOVING_SUM("k", "v", 256).values[0];
        eq(a1[0], 1, "the first partial window is the element itself");
        eq(a1[100], 101, "a partial window at 100 holds 101 elements");
        eq(a1[255], 256, "the first FULL window holds 256");
        eq(a1[m - 1], 256, "and later windows stay 256");
        const b1 = bf.GROUP_ARRAY_MOVING_AVG("k", "v", 256).values[0];
        ok(b1[0] === 1 && b1[100] === 1 && b1[m - 1] === 1,
           "every mean of an all-ones column is exactly 1");
    }
    throwsLike(() => df.GROUP_ARRAY_MOVING_SUM("k", "v", 0), "positive integer",
               "a zero window is refused");
    throwsLike(() => df.GROUP_ARRAY_MOVING_AVG("k", "v", 2.5), "positive integer",
               "a fractional window is refused");
    throwsLike(() => df.GROUP_ARRAY_MOVING_SUM("k", "nope"), "no such column",
               "an unknown value column is refused");
}

S("covariance matrix");
{
    mark("COV_MATRIX");
    const a = Float64Array.from([1, 2, 3, 4, 5, 7]);
    const b = Float64Array.from([2, 4, 7, 8, 11, 13]);
    const df = new DataFrame({ a, b });
    const cm = df.COV_MATRIX(["a", "b"]);
    eq(cm.n, 2, "COV_MATRIX reports its own order");
    elemEq(cm.columns, ["a", "b"], "and echoes the column names");
    /* a cell must equal the pairwise call to the last bit, not merely closely */
    eq(cm.matrix[1], df.COV_SAMP("a", "b"), "the off-diagonal IS COV_SAMP");
    eq(cm.matrix[2], cm.matrix[1], "and the matrix is symmetric");
    /* The diagonal IS the pairwise call, exactly. It is NOT bit-equal to
       VARIANCE -- two summation orders, diverging above DFM_UNROLL_MIN
       (0.7 ulp at n=63), so eq() would hold only for frames under 64. */
    eq(cm.matrix[0], df.COV_SAMP("a", "a"), "the diagonal is COV_SAMP(a,a), exactly");
    eq(cm.matrix[3], df.COV_SAMP("b", "b"), "for every column");
    ulpNear(cm.matrix[0], df.VARIANCE("a"), 4, "and is the sample variance to within 4 ulp");
    const one = df.COV_MATRIX(["a"]);
    eq(one.matrix[0], df.COV_SAMP("a", "a"), "a 1x1 matrix is just that variance");
    /* the divergence is real, so cross the threshold rather than assume it */
    {
        const n = 200, big = new Float64Array(n);
        for (let i = 0; i < n; i++) big[i] = ((i * 7919) % 1013) - 506 + i * 0.03125;
        const bf = new DataFrame({ big });
        eq(bf.COV_MATRIX(["big"]).matrix[0], bf.COV_SAMP("big", "big"),
           "above DFM_UNROLL_MIN the diagonal is still exactly COV_SAMP");
        ulpNear(bf.COV_MATRIX(["big"]).matrix[0], bf.VARIANCE("big"), 8,
                "and still within 8 ulp of VARIANCE");
    }
    throwsLike(() => df.COV_MATRIX("a"), "array", "COV_MATRIX refuses a bare name");
    throwsLike(() => df.COV_MATRIX([]), "between 1", "and refuses an empty list");
    /* A repeated NAME must give a repeated CELL, not a second computation:
       1024 names over a 2-column frame was 524288 pair scans, ~128 s. */
    {
        const rep = ["a", "b", "a", "b", "a"];
        const rm2 = df.CORR_MATRIX(rep), cm2 = df.COV_MATRIX(rep);
        eq(rm2.n, 5, "a repeated name still yields one row and column each");
        elemEq(rm2.columns, rep, "and echoes the names as given");
        for (let i = 0; i < 5; i++)
            for (let j = 0; j < 5; j++) {
                const same = rep[i] === rep[j];
                if (same)
                    eq(rm2.matrix[i * 5 + j], 1, "identical columns correlate 1 at (" + i + "," + j + ")");
                eq(rm2.matrix[i * 5 + j], rm2.matrix[j * 5 + i], "symmetric at (" + i + "," + j + ")");
                eq(cm2.matrix[i * 5 + j], cm2.matrix[j * 5 + i], "COV symmetric at (" + i + "," + j + ")");
            }
        /* every duplicate cell equals the pairwise call, so the collapse did
           not just make it fast, it kept it right */
        eq(rm2.matrix[1], df.CORR("a", "b"), "a duplicated off-diagonal is still CORR");
        eq(rm2.matrix[3], df.CORR("a", "b"), "and so is its repeat");
        eq(cm2.matrix[1], df.COV_SAMP("a", "b"), "COV's duplicated cell is COV_SAMP");
        eq(cm2.matrix[0], df.COV_SAMP("a", "a"), "and its repeated diagonal is too");
        /* rep = [a,b,a,b,a] and nc = 5, so (2,2) is index 12 -- index 8 is
           (1,3), which is ("b","b") and a different number entirely. */
        eq(cm2.matrix[12], df.COV_SAMP("a", "a"), "at every repeat of that column");
        eq(cm2.matrix[8], df.COV_SAMP("b", "b"), "and the other column repeats too");
    }
    /* CORR_MATRIX must be unchanged by COV_MATRIX sharing its implementation */
    const rm = df.CORR_MATRIX(["a", "b"]);
    eq(rm.matrix[0], 1, "CORR_MATRIX still has an exact 1 on the diagonal");
    eq(rm.matrix[1], df.CORR("a", "b"), "and its off-diagonal is still CORR");
    /* The diagonal used to be a literal 1.0 — the one cell not computed from
       the moments — so it read 1 for a CONSTANT column while CORR(c,c) is NaN.
       Every cell must agree with the pairwise call, including this one. */
    {
        const k = new DataFrame({
            c: Float64Array.from([5, 5, 5, 5]),
            v: Float64Array.from([1, 2, 3, 4]),
        });
        ok(Number.isNaN(k.CORR("c", "c")), "CORR of a constant column is NaN");
        ok(Number.isNaN(k.CORR_MATRIX(["c"]).matrix[0]),
           "so the CORR_MATRIX diagonal is NaN too, not a literal 1",
           "got " + k.CORR_MATRIX(["c"]).matrix[0]);
        const mixed = k.CORR_MATRIX(["c", "v"]);
        ok(Number.isNaN(mixed.matrix[0]), "the constant column's diagonal is NaN");
        eq(mixed.matrix[3], 1, "the varying column's diagonal is still exactly 1");
        /* COV has a real answer for a constant column: zero variance. */
        eq(k.COV_MATRIX(["c"]).matrix[0], k.COV_SAMP("c", "c"),
           "COV_MATRIX's diagonal is 0 there, and equals COV_SAMP");
    }
    /* A column name may hold U+0000; the C name is NUL-terminated, so strcmp
       would match a PREFIX and "secret\0ignored" would resolve to "secret". */
    {
        const NUL = String.fromCharCode(0);
        const g = new DataFrame({ secret: Float64Array.from([1, 2, 3]) });
        eq(g.SUM("secret"), 6, "the real name still resolves");
        throwsLike(() => g.SUM("secret" + NUL + "ignored"), "NUL",
                   "a name with a trailing NUL segment is refused, not truncated");
        throwsLike(() => g.SUM("sec" + NUL + "ret"), "NUL",
                   "and so is one with an interior NUL");
        throwsLike(() => g.COV_MATRIX(["secret" + NUL + "x"]), "NUL",
                   "the array-of-names path refuses it too");
        /* The CONSTRUCTOR truncated too, and that is the sharper case: two
           distinct keys became one name, so COLUMNS read ["a","a"] and the
           second column could never be addressed again. */
        const two = {};
        two["a" + NUL + "b"] = Float64Array.from([1, 1, 1]);
        two["a" + NUL + "c"] = Float64Array.from([9, 9, 9]);
        throwsLike(() => new DataFrame(two), "NUL",
                   "the constructor refuses a NUL column name");
        const one = {};
        one["a" + NUL + "b"] = Float64Array.from([1, 2, 3]);
        throwsLike(() => new DataFrame(one), "NUL",
                   "even when only one column would collide with nothing");
    }
}

/* ============================== frame-returning verbs (round 6) =========== */

/* Helpers: build a small frame and read it back. TO_RECORDS is the oracle the
   frame verbs are tested against -- a join's output must be read as objects,
   not re-derived from the frames that fed it. */
function records(df) { return df.TO_RECORDS(); }

S("interop");
{
    const df = new DataFrame({
        a: Float64Array.from([1, 2, 3]),
        b: Int32Array.from([4, 5, 6]),
        s: ["x", "y", "z"],
    });
    eq(df.DTYPES().a, "f64", "DTYPES f64");
    eq(df.DTYPES().b, "i32", "DTYPES i32");
    eq(df.DTYPES().s, "str", "DTYPES str");
    const sc = df.SCHEMA();
    eq(sc.length, 3, "SCHEMA length");
    eq(sc[0].name, "a", "SCHEMA name 0");
    eq(sc[1].type, "i32", "SCHEMA type 1");
    const info = df.INFO();
    eq(info.rows, 3, "INFO rows");
    eq(info.cols, 3, "INFO cols");
    eq(info.bytes.a, 24, "INFO bytes a (3 f64)");
    eq(info.bytes.b, 12, "INFO bytes b (3 i32)");
    ok(info.total_bytes > 0, "INFO total_bytes positive", info.total_bytes);
    const mu = df.MEMORY_USAGE();
    eq(mu.columns.a, 24, "MEMORY_USAGE a");
    eq(mu.total, info.total_bytes, "MEMORY_USAGE total matches INFO");

    const tc = df.TO_COLUMNS();
    ok(tc.a instanceof Float64Array, "TO_COLUMNS a is Float64Array");
    ok(tc.b instanceof Int32Array, "TO_COLUMNS b keeps its exact type");
    eq(tc.a[1], 2, "TO_COLUMNS a[1]");
    ok(Array.isArray(tc.s), "TO_COLUMNS string column is an Array");
    eq(tc.s[2], "z", "TO_COLUMNS s[2]");
    /* A copy must NOT alias: writing the result must not touch the source. */
    tc.a[0] = 999;
    eq(df.TO_COLUMNS().a[0], 1, "TO_COLUMNS copies, does not alias");

    const tr = records(df);
    eq(tr.length, 3, "TO_RECORDS length");
    eq(tr[1].b, 5, "TO_RECORDS row object fields");
    eq(tr[2].s, "z", "TO_RECORDS string value");

    eq(df.TO_JSON(), '[{"a":1,"b":4,"s":"x"},{"a":2,"b":5,"s":"y"},{"a":3,"b":6,"s":"z"}]',
       "TO_JSON is the records serialised");

    /* CSV: header, quoting of a field holding a comma, and NaN -> empty. */
    const df2 = new DataFrame({
        x: Float64Array.from([1.5, NaN]),
        y: ["a,b", "plain"],
    });
    {
        const csv = df2.TO_CSV();
        const lines = csv.split("\n");
        eq(lines[0], "x,y", "TO_CSV header row");
        eq(lines[1], '1.5,"a,b"', "TO_CSV quotes a field with a comma");
        ok(lines[2].indexOf("plain") >= 0, "TO_CSV unquoted plain field", lines[2]);
    }

    /* FROM_RECORDS: column union in first-seen order, numeric vs string, and a
       missing key filling NaN / "". The receiver is ignored. */
    const fr = df.FROM_RECORDS([
        { x: 1, y: "a" },
        { x: 2, y: "b" },
        { y: "c" },
        { x: 4, z: 9 },
    ]);
    eq(fr.COLUMNS.join(","), "x,y,z", "FROM_RECORDS column union order");
    ok(fr.DTYPES().x === "f64" && fr.DTYPES().y === "str" && fr.DTYPES().z === "f64",
       "FROM_RECORDS per-column type inference");
    const frr = records(fr);
    eq(frr.length, 4, "FROM_RECORDS row count");
    ok(Number.isNaN(frr[2].x), "FROM_RECORDS missing numeric key is NaN");
    eq(frr[2].y, "c", "FROM_RECORDS present value kept");
    ok(Number.isNaN(frr[0].z), "FROM_RECORDS missing z in row 0 is NaN");
    eq(frr[3].z, 9, "FROM_RECORDS z present in row 3");
    throwsLike(() => df.FROM_RECORDS([]), "zero rows",
               "FROM_RECORDS refuses an empty array");
    throwsLike(() => df.FROM_RECORDS([1, 2]), "not a plain",
               "FROM_RECORDS refuses non-object rows");

    /* COPY: exact types, no aliasing. */
    const cp = df.COPY();
    eq(cp.COLUMNS.join(","), df.COLUMNS.join(","), "COPY same columns");
    ok(cp.TO_COLUMNS().b instanceof Int32Array, "COPY keeps integer type");
    cp.TO_COLUMNS().b[0] = 111;
    eq(df.TO_COLUMNS().b[0], 4, "COPY does not alias the source");
    mark("DTYPES", "SCHEMA", "INFO", "MEMORY_USAGE", "TO_COLUMNS", "TO_RECORDS",
         "TO_JSON", "TO_CSV", "FROM_RECORDS", "COPY");
}

S("select/slice");
{
    const df = new DataFrame({
        a: Float64Array.from([1, 2, 3]),
        b: Int32Array.from([4, 5, 6]),
        s: ["x", "y", "z"],
    });
    eq(df.SELECT(["b", "s"]).COLUMNS.join(","), "b,s",
       "SELECT keeps the requested order");
    ok(df.SELECT(["b"]).TO_COLUMNS().b instanceof Int32Array,
       "SELECT keeps the selected column's type");
    throwsLike(() => df.SELECT(["a", "a"]), "twice",
               "SELECT refuses a duplicate name");
    throwsLike(() => df.SELECT(["nope"]), "no such column",
               "SELECT refuses an unknown name");

    eq(df.DROP_COLUMNS(["b"]).COLUMNS.join(","), "a,s",
       "DROP_COLUMNS keeps the complement in order");
    throwsLike(() => df.DROP_COLUMNS(["b", "b"]), "twice",
               "DROP_COLUMNS refuses a duplicate");
    throwsLike(() => df.DROP_COLUMNS(["nope"]), "no such column",
               "DROP_COLUMNS refuses an unknown name");

    eq(df.RENAME({ a: "aa" }).COLUMNS.join(","), "aa,b,s",
       "RENAME keeps position and renames in place");
    eq(df.RENAME({ a: "aa", s: "ss" }).COLUMNS.join(","), "aa,b,ss",
       "RENAME multiple entries");
    throwsLike(() => df.RENAME({ nope: "x" }), "no such column",
               "RENAME refuses an unknown source");
    throwsLike(() => df.RENAME({ a: "b" }), "collides",
               "RENAME refuses a target colliding with an unrenamed column");

    const mask = new Uint8Array([1, 0, 1]);
    const fl = df.FILTER(mask);
    eq(fl.ROWS, 2, "FILTER row count");
    eq(records(fl)[0].a, 1, "FILTER keeps masked rows in order");
    eq(records(fl)[1].s, "z", "FILTER second kept row");
    throwsLike(() => df.FILTER(new Uint8Array(2)), "at least",
               "FILTER refuses a too-short mask");

    const sl = df.SLICE(1, 3);
    eq(sl.ROWS, 2, "SLICE row count");
    eq(sl.TO_COLUMNS().a[0], 2, "SLICE start inclusive");
    eq(df.SLICE(1).TO_COLUMNS().a[0], 2, "SLICE default end is nrows");
    eq(df.SLICE(-2).ROWS, 2, "SLICE negative start counts from the end");
    eq(df.SLICE(0, 0).ROWS, 0, "SLICE empty range");

    /* SAMPLE: same seed -> same sample; without replacement. */
    const sa1 = df.SAMPLE(2, 42), sa2 = df.SAMPLE(2, 42);
    eq(sa1.TO_COLUMNS().a[0], sa2.TO_COLUMNS().a[0], "SAMPLE reproducible for a seed");
    eq(sa1.TO_COLUMNS().a[1], sa2.TO_COLUMNS().a[1], "SAMPLE seed stable second row");
    eq(sa1.ROWS, 2, "SAMPLE n rows");
    throwsLike(() => df.SAMPLE(4), "exceeds",
               "SAMPLE refuses n > nrows");
    throwsLike(() => df.SAMPLE(1.5), "integer",
               "SAMPLE refuses a non-integer n");
    /* Without replacement: n rows sampled from 3 distinct is 3 distinct rows. */
    {
        const big = new DataFrame({ v: Float64Array.from([0, 1, 2, 3, 4, 5]) });
        const got = big.SAMPLE(6, 7).TO_COLUMNS().v;
        const set = new Set(Array.from(got));
        eq(set.size, 6, "SAMPLE without replacement gives distinct rows");
    }

    eq(df.ISIN("a", [2, 3]).join(","), "0,1,1", "ISIN numeric membership");
    eq(df.ISIN("s", ["x"]).join(","), "1,0,0", "ISIN string membership");
    eq(df.ISIN("a", []).join(","), "0,0,0", "ISIN empty values -> all zero");
    throwsLike(() => df.ISIN("a", "notarray"), "array",
               "ISIN refuses a non-array");

    const mk = df.MASK(new Uint8Array([1, 0, 1]), 0);
    eq(mk.TO_COLUMNS().a[1], 0, "MASK fills a masked-out numeric row");
    eq(mk.TO_COLUMNS().s[1], "", "MASK fills a masked-out string row");
    const mkd = df.MASK(new Uint8Array([1, 0, 1]));
    ok(Number.isNaN(mkd.TO_COLUMNS().a[1]), "MASK default fill is NaN");
    mark("SELECT", "DROP_COLUMNS", "RENAME", "FILTER", "SLICE", "SAMPLE",
         "ISIN", "MASK");
}

S("join");
{
    /* The definition: an inner join's rows are the (left, right) pairs whose
       keys are equal, in left-major order; a duplicate key on the right
       multiplies the row. */
    const L = new DataFrame({
        k: Int32Array.from([1, 2, 2, 4]),
        lv: Float64Array.from([1, 2, 3, 4]),
    });
    const R = new DataFrame({
        k: Int32Array.from([2, 2, 3]),
        rv: Float64Array.from([20, 21, 30]),
    });
    const inner = records(L.JOIN(R, "k", "k"));
    eq(inner.length, 4, "JOIN inner: 2 left rows x 2 right matches");
    eq(inner[0].lv + inner[0].rv, 22, "JOIN inner first pair");
    eq(inner[3].lv, 3, "JOIN inner last left row");
    eq(inner[3].rv, 21, "JOIN inner last right row");
    eq(L.JOIN(R, "k", "k").COLS, 4, "JOIN carries both sides");
    eq(L.JOIN(R, "k", "k").COLUMNS.join(","), "k,lv,k_right,rv",
       "JOIN suffix-collides the right key");

    const left = records(L.JOIN(R, "k", "k", "left"));
    eq(left.length, 6, "JOIN left keeps every left row (1 + 2x2 + 1)");
    ok(Number.isNaN(left[0].rv), "JOIN left unmatched right is NaN");
    eq(left[1].rv, 20, "JOIN left matched row");
    eq(left[0].k, 1, "JOIN left unmatched left key");
    eq(left[4].rv, 21, "JOIN left second duplicate key's second match");

    const right = records(L.JOIN(R, "k", "k", "right"));
    eq(right.length, 5, "JOIN right keeps every right row (2x2 + 1)");
    eq(right[0].rv, 20, "JOIN right first right value");
    eq(right[4].k_right, 3, "JOIN right unmatched right key");
    ok(Number.isNaN(right[4].lv), "JOIN right unmatched left is NaN");
    eq(right[4].rv, 30, "JOIN right unmatched right value");

    const outer = records(L.JOIN(R, "k", "k", "outer"));
    eq(outer.length, 7, "JOIN outer union row count");
    eq(outer[outer.length - 1].matched, 1, "JOIN outer right-only row matched=1");
    eq(outer[0].matched, 0, "JOIN outer left-only row matched=0");
    ok(Number.isNaN(outer[0].rv), "JOIN outer left-only right NaN");

    throwsLike(() => L.JOIN(R, "k", "k", "cross"), "how",
               "JOIN refuses an unknown how");
    const F = new DataFrame({ f: Float64Array.from([1.5, 2.5]) });
    throwsLike(() => L.JOIN(F, "k", "f"), "integer",
               "JOIN refuses a float key");
    const S = new DataFrame({ k: ["a", "b"] });
    throwsLike(() => L.JOIN(S, "k", "k"), "integer",
               "JOIN refuses a string key");

    /* A string column carried on a side that can be missing is refused. */
    const SL = new DataFrame({ k: Int32Array.from([1, 2]), s: ["p", "q"] });
    const SR = new DataFrame({ k: Int32Array.from([2]), s: ["q"] });
    throwsLike(() => L.JOIN(SR, "k", "k", "left"), "string",
               "JOIN left refuses a right string column");
    throwsLike(() => L.JOIN(SR, "k", "k", "outer"), "string",
               "JOIN outer refuses a right string column");
    /* But an inner join carries it fine (no side is missing). */
    const sR = new DataFrame({ k: Int32Array.from([2]), s: ["q"], rv: Float64Array.from([9]) });
    eq(records(SL.JOIN(sR, "k", "k"))[0].s, "q", "JOIN inner carries the string column");

    /* Empty join: no matches -> inner is empty, left keeps all rows. */
    const E = new DataFrame({ k: Int32Array.from([10, 11]), v: Float64Array.from([1, 2]) });
    eq(L.JOIN(E, "k", "k").ROWS, 0, "JOIN inner with no matches is empty");
    eq(L.JOIN(E, "k", "k", "left").ROWS, 4, "JOIN left with no matches keeps all");
    /* Duplicate right keys past the index table's first rehash (32 entries)
       must stay in RIGHT-ROW order: the rehash once reversed every chain and
       silently reordered matched rows. */
    {
        const N = 64;
        const L7 = new DataFrame({ k: Int32Array.from([7]), lv: Float64Array.from([1]) });
        const dup = new DataFrame({
            k: new Int32Array(N).fill(7),
            rv: Float64Array.from(Array.from({ length: N }, (_, i) => i)),
        });
        const many = L7.JOIN(dup, "k", "k");
        eq(many.ROWS, N, "JOIN with 64 duplicate right keys keeps every match");
        const rv = many.TO_COLUMNS().rv;
        let ordered = true;
        for (let i = 0; i < N; i++) if (rv[i] !== i) { ordered = false; break; }
        ok(ordered, "JOIN 64 duplicate matches preserve right-row order",
           "rv[0]=" + rv[0] + " rv[63]=" + rv[63]);
    }
    /* A HASH COLLISION must not leak another key's rows into the match: keys 3
       and 13 land in the same bucket, so the join has to compare keys, not
       trust the bucket head. A lookup for 3 must never emit 13's rows. */
    {
        const Lc = new DataFrame({ k: Int32Array.from([3]), lv: Float64Array.from([100]) });
        const Rc = new DataFrame({
            k: Int32Array.from([3, 3, 13, 13]),
            rv: Float64Array.from([1, 2, 3, 4]),
        });
        const got = Lc.JOIN(Rc, "k", "k");
        eq(got.ROWS, 2, "JOIN collision: only key 3's rows match");
        eq(got.TO_COLUMNS().rv.join(","), "1,2",
           "JOIN collision: key 13's rows are not emitted for key 3");
        const Rc2 = new DataFrame({
            k: Int32Array.from([13, 13, 3, 3]),
            rv: Float64Array.from([1, 2, 3, 4]),
        });
        const got2 = Lc.JOIN(Rc2, "k", "k");
        eq(got2.ROWS, 2, "JOIN collision works with the other insertion order");
        eq(got2.TO_COLUMNS().rv.join(","), "3,4",
           "JOIN collision: right-row order preserved across the collision");
        /* Both keys present on both sides, interleaved. */
        const Ld = new DataFrame({ k: Int32Array.from([3, 13]), lv: Float64Array.from([1, 2]) });
        const gd = Ld.JOIN(Rc, "k", "k");
        eq(gd.ROWS, 4, "JOIN collision with both keys present: 4 pairs");
        eq(gd.TO_COLUMNS().rv.slice(0, 2).join(","), "1,2",
           "JOIN collision: key 3 matched before key 13");
    }
    mark("JOIN");
}

S("asof_join");
{
    const AT = new DataFrame({
        t: Int32Array.from([1, 3, 5, 8]),
        av: Float64Array.from([10, 30, 50, 80]),
    });
    const BT = new DataFrame({
        t: Int32Array.from([2, 4, 9]),
        bv: Float64Array.from([20, 40, 90]),
    });
    const r = records(AT.ASOF_JOIN(BT, "t", "t"));
    eq(r.length, 4, "ASOF_JOIN keeps every left row");
    eq(r[0].av, 10, "ASOF left value kept");
    ok(Number.isNaN(r[0].bv), "ASOF first left row has no preceding right");
    eq(r[1].bv, 20, "ASOF t=3 matches right t=2");
    eq(r[2].bv, 40, "ASOF t=5 matches right t=4");
    eq(r[3].bv, 40, "ASOF t=8 matches right t=4 (nearest PRECEDING)");

    /* Unsorted input is refused on both sides. */
    const U = new DataFrame({ t: Int32Array.from([3, 1, 2]), v: Float64Array.from([1, 2, 3]) });
    throwsLike(() => AT.ASOF_JOIN(U, "t", "t"), "sorted",
               "ASOF_JOIN refuses an unsorted right time column");
    throwsLike(() => U.ASOF_JOIN(BT, "t", "t"), "sorted",
               "ASOF_JOIN refuses an unsorted left time column");
    /* A tie in right times keeps the nearest, which is the LAST equal one. */
    const TT = new DataFrame({ t: Int32Array.from([5, 5]), v: Float64Array.from([50, 51]) });
    eq(records(AT.ASOF_JOIN(TT, "t", "t"))[2].v, 51,
       "ASOF_JOIN with a right tie takes the last equal time");
    mark("ASOF_JOIN");
}

S("concat");
{
    const C1 = new DataFrame({ a: Float64Array.from([1, 2]), s: ["p", "q"] });
    const C2 = new DataFrame({ a: Float64Array.from([3]), s: ["p"] });
    const r = records(C1.CONCAT(C2));
    eq(r.length, 3, "CONCAT stacks rows");
    eq(r[0].a, 1, "CONCAT first frame first");
    eq(r[2].a, 3, "CONCAT second frame second");
    /* The dictionary is merged: "p" appears on both sides but once in the
       output, and codes round-trip to the same string. */
    eq(r[0].s, "p", "CONCAT left string value");
    eq(r[2].s, "p", "CONCAT right string value merged to the same entry");

    throwsLike(() => C1.CONCAT(new DataFrame({ a: Float64Array.from([1]), x: Float64Array.from([2]) })),
               "match in order", "CONCAT refuses a different column set");
    throwsLike(() => C1.CONCAT(new DataFrame({ s: ["x"], a: Float64Array.from([1]) })),
               "column 0 is", "CONCAT refuses a reordered column set");
    const N = new DataFrame({ a: ["x"], s: Float64Array.from([1]) });
    throwsLike(() => C1.CONCAT(N), "string", "CONCAT refuses string vs numeric");

    /* Same integer type stays integer; mixed types widen to f64. */
    const I1 = new DataFrame({ v: Int32Array.from([1, 2]) });
    const I2 = new DataFrame({ v: Int32Array.from([3]) });
    ok(I1.CONCAT(I2).TO_COLUMNS().v instanceof Int32Array,
       "CONCAT of two same-type int columns stays int");
    const F = new DataFrame({ v: Float64Array.from([4]) });
    ok(I1.CONCAT(F).TO_COLUMNS().v instanceof Float64Array,
       "CONCAT of mixed numeric types widens to f64");
    mark("CONCAT");
}

S("resample");
{
    const TS = new DataFrame({
        t: Float64Array.from([0, 1, 10, 11, 20]),
        v: Float64Array.from([1, 2, 3, 4, 5]),
    });
    /* Buckets are half-open [k*10, (k+1)*10); t=20 starts its own bucket. The
       time column is aggregated (sum of the time column per bucket). */
    const r = records(TS.RESAMPLE("t", 10));
    eq(r.length, 3, "RESAMPLE occupied buckets only");
    eq(r[0].bucket, 0, "RESAMPLE first bucket aligned to a multiple of 10");
    eq(r[0].value, 1, "RESAMPLE sum of time 0+1");
    eq(r[1].bucket, 10, "RESAMPLE second bucket start");
    eq(r[1].value, 21, "RESAMPLE sum of time 10+11");
    eq(r[2].value, 20, "RESAMPLE last bucket is t=20 alone");
    /* A bucket start is aligned DOWN, so a first time of 3 lands in [0,10). */
    const T2 = new DataFrame({ t: Float64Array.from([3, 4]), v: Float64Array.from([1, 1]) });
    eq(records(T2.RESAMPLE("t", 10))[0].bucket, 0,
       "RESAMPLE aligns the first bucket down to a multiple of interval");
    eq(records(T2.RESAMPLE("t", 10, "count"))[0].value, 2,
       "RESAMPLE count aggregate");
    eq(records(TS.RESAMPLE("t", 10, "mean"))[0].value, 0.5,
       "RESAMPLE mean aggregate (0+1)/2");

    throwsLike(() => TS.RESAMPLE("t", 0), "positive",
               "RESAMPLE refuses a non-positive interval");
    const U = new DataFrame({ t: Float64Array.from([3, 1, 2]) });
    throwsLike(() => U.RESAMPLE("t", 1), "sorted",
               "RESAMPLE refuses an unsorted time column");
    throwsLike(() => TS.RESAMPLE("t", 1, "median"), "sum/mean/min/max/count",
               "RESAMPLE refuses an unknown aggregate");
    /* Empty frame: still the two-column shape, zero rows. */
    const E = new DataFrame({ t: new Float64Array(0) });
    eq(E.RESAMPLE("t", 5).ROWS, 0, "RESAMPLE of an empty frame is empty");
    eq(E.RESAMPLE("t", 5).COLS, 2, "RESAMPLE of an empty frame has 2 columns");
    mark("RESAMPLE");
}

S("pivot");
{
    const PV = new DataFrame({
        id: Int32Array.from([1, 1, 2, 2, 2]),
        grp: ["a", "b", "a", "a", "b"],
        val: Float64Array.from([10, 20, 30, 40, 50]),
    });
    const piv = PV.PIVOT("id", "grp", "val");
    eq(piv.COLUMNS.join(","), "id,a,b", "PIVOT columns: index then pivot values");
    const r = records(piv);
    eq(r.length, 2, "PIVOT one row per distinct index");
    eq(r[0].a, 10, "PIVOT cell with one row");
    eq(r[0].b, 20, "PIVOT second cell");
    eq(r[1].a, 70, "PIVOT sum over two colliding rows (30+40)");
    eq(r[1].b, 50, "PIVOT sum over one row");

    /* A cell with no rows is NaN for sum. */
    const PV2 = new DataFrame({
        id: Int32Array.from([1, 2]),
        grp: ["a", "b"],
        val: Float64Array.from([10, 20]),
    });
    const r2 = records(PV2.PIVOT("id", "grp", "val"));
    ok(Number.isNaN(r2[0].b), "PIVOT empty cell is NaN (sum)");
    ok(Number.isNaN(r2[1].a), "PIVOT empty cell NaN on the other diagonal");
    eq(records(PV2.PIVOT("id", "grp", "val", "count"))[0].a, 1, "PIVOT count");
    eq(records(PV2.PIVOT("id", "grp", "val", "count"))[0].b, 0,
       "PIVOT count of an empty cell is 0");

    /* min/max/first/last aggregates, and NaN values ignored by min/max. */
    {
        const M = new DataFrame({
            id: Int32Array.from([1, 1, 2]),
            grp: ["a", "a", "b"],
            val: Float64Array.from([5, NaN, 9]),
        });
        eq(records(M.PIVOT("id", "grp", "val", "min"))[0].a, 5, "PIVOT min ignores NaN");
        eq(records(M.PIVOT("id", "grp", "val", "max"))[0].a, 5, "PIVOT max of a lone value");
        eq(records(M.PIVOT("id", "grp", "val", "first"))[0].a, 5, "PIVOT first non-NaN");
        eq(records(M.PIVOT("id", "grp", "val", "last"))[0].a, 5, "PIVOT last is the non-NaN");
        ok(Number.isNaN(records(M.PIVOT("id", "grp", "val", "last"))[0].b),
           "PIVOT empty cell is NaN for last");
    }

    throwsLike(() => PV.PIVOT("id", "grp", "val", "median"), "agg",
               "PIVOT refuses an unknown aggregate");
    /* A pivot value whose string equals the index column NAME is refused. */
    {
        const C = new DataFrame({
            a: Float64Array.from([1, 2]),
            grp: ["x", "a"],
            val: Float64Array.from([10, 20]),
        });
        throwsLike(() => C.PIVOT("a", "grp", "val"), "collides",
                   "PIVOT refuses a pivot value colliding with the index name");
    }
    /* A string index column is allowed (group-by supports it). */
    const S = new DataFrame({
        id: ["u", "u", "v"],
        grp: ["a", "b", "a"],
        val: Float64Array.from([1, 2, 3]),
    });
    const sr = records(S.PIVOT("id", "grp", "val"));
    eq(sr.length, 2, "PIVOT with a string index");
    eq(sr[0].id, "u", "PIVOT string index value kept");
    eq(sr[0].a, 1, "PIVOT string-index cell");
    mark("PIVOT");
}

S("melt");
{
    const ML = new DataFrame({
        id: Int32Array.from([1, 2]),
        x: Float64Array.from([1, 2]),
        y: Float64Array.from([3, 4]),
    });
    const r = records(ML.MELT(["id"], ["x", "y"]));
    eq(r.length, 4, "MELT rows = nrows * valueVars");
    eq(r[0].variable, "x", "MELT first valueVar name");
    eq(r[0].value, 1, "MELT value at row 0 var x");
    eq(r[1].variable, "y", "MELT second valueVar");
    eq(r[2].id, 2, "MELT id repeats per valueVar");
    eq(r[3].value, 4, "MELT last cell");

    throwsLike(() => ML.MELT(["id"], []), "empty",
               "MELT refuses empty valueVars");
    throwsLike(() => ML.MELT(["x"], ["x"]), "both",
               "MELT refuses a column in both lists");
    const SM = new DataFrame({ id: ["a", "b"], x: Float64Array.from([1, 2]) });
    eq(records(SM.MELT(["id"], ["x"]))[0].id, "a",
       "MELT carries a string id column");
    mark("MELT");
}

S("json_agg");
{
    const JG = new DataFrame({
        k: ["a", "b", "a"],
        v: Float64Array.from([1, 2, 3]),
    });
    eq(JG.JSON_AGG("k", "v"), '{"a":[1,3],"b":[2]}',
       "JSON_AGG groups values into arrays");
    eq(JG.JSON_OBJECT_AGG("k", "v"), '{"a":3,"b":2}',
       "JSON_OBJECT_AGG keeps the LAST value of a duplicate key");
    /* A string key that would walk the prototype chain is an OWN property. */
    const P = new DataFrame({
        k: ["__proto__", "constructor"],
        v: Float64Array.from([1, 2]),
    });
    eq(P.JSON_OBJECT_AGG("k", "v"), '{"__proto__":1,"constructor":2}',
       "JSON_OBJECT_AGG defines '__proto__' as an own property");
    eq(P.JSON_AGG("k", "v"), '{"__proto__":[1],"constructor":[2]}',
       "JSON_AGG same for the array form");
    /* NaN/Infinity -> null in the plain forms, refused by _STRICT. */
    const JN = new DataFrame({
        k: ["a", "b"],
        v: Float64Array.from([1, NaN]),
    });
    eq(JN.JSON_AGG("k", "v"), '{"a":[1],"b":[null]}',
       "JSON_AGG serialises NaN as null");
    eq(JN.JSON_OBJECT_AGG("k", "v"), '{"a":1,"b":null}',
       "JSON_OBJECT_AGG serialises NaN as null");
    throwsLike(() => JN.JSON_AGG_STRICT("k", "v"), "non-finite",
               "JSON_AGG_STRICT refuses NaN");
    throwsLike(() => JN.JSON_OBJECT_AGG_STRICT("k", "v"), "non-finite",
               "JSON_OBJECT_AGG_STRICT refuses NaN");
    const JI = new DataFrame({ k: ["a"], v: Float64Array.from([Infinity]) });
    eq(JI.JSON_AGG("k", "v"), '{"a":[null]}',
       "JSON_AGG serialises Infinity as null");

    /* Numeric keys group by exact value. */
    const JN2 = new DataFrame({
        k: Float64Array.from([1, 1.0, 2]),
        v: Float64Array.from([10, 20, 30]),
    });
    eq(JN2.JSON_AGG("k", "v"), '{"1":[10,20],"2":[30]}',
       "JSON_AGG numeric keys: 1 and 1.0 are the same key");

    /* Mask: excluded rows do not appear. */
    const JMA = JG.JSON_AGG("k", "v", new Uint8Array([1, 1, 0]));
    eq(JMA, '{"a":[1],"b":[2]}', "JSON_AGG honours the mask");
    /* An empty frame has no keys: BOTH forms are the empty OBJECT, and must
       not be re-stringified into a quoted string. */
    {
        const E = new DataFrame({ k: [], v: new Float64Array(0) });
        eq(E.JSON_AGG("k", "v"), "{}", "JSON_AGG on an empty frame is {}");
        eq(E.JSON_OBJECT_AGG("k", "v"), "{}", "JSON_OBJECT_AGG on an empty frame is {}");
    }
    mark("JSON_AGG", "JSON_OBJECT_AGG", "JSON_AGG_STRICT", "JSON_OBJECT_AGG_STRICT");
}

/* ============================================ every method was exercised */
S("coverage");
{
    const proto = Object.getPrototypeOf(new DataFrame({ a: new Float64Array(1) }));
    const methods = Object.getOwnPropertyNames(proto)
        .filter((k) => k !== "constructor" &&
                       typeof Object.getOwnPropertyDescriptor(proto, k).value === "function")
        .sort();
    const missing = methods.filter((m) => !touched.has(m));
    ok(missing.length === 0,
       "every method on DataFrame.prototype is exercised above",
       "NOT COVERED: " + missing.join(", "));
    /* The stronger axis: actually CALLED, recorded by the prototype wrapper, so
       it cannot be satisfied by typing a name in a mark() list. */
    const never = methods.filter((m) => !invoked.has(m));
    ok(never.length === 0,
       "every method on DataFrame.prototype was actually INVOKED",
       "NEVER CALLED: " + never.join(", "));
    ok(invoked.size > 0, "the invocation wrapper is live at all",
       "invoked " + invoked.size + " distinct methods");
    ok(methods.length > 0, "the prototype enumeration found methods at all",
       "found " + methods.length);
    console.log("  " + methods.length + " methods on the prototype, " +
                (methods.length - missing.length) + " covered");
}

console.log("test_dataframe: " + pass + " passed, " + fail + " failed");
if (fail) {
    console.log("--- failures ---");
    for (const f of failures) console.log(f);
    throw new Error(fail + " dataframe test(s) failed");
}
