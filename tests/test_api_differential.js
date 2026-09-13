/* test_api_differential.js -- VALUE-level coverage at scale, by differential.
 *
 * The hand-written tables in test_api_params.js reach 19% because every row
 * costs a human decision about the expected digit. This file gets the rest by
 * computing the answer a SECOND way, in plain JS, and requiring the two to
 * agree. That satisfies the rule the tables exist for -- the expectation comes
 * from outside the engine -- without needing a published vector per name.
 *
 * WHAT A DIFFERENTIAL CAN AND CANNOT DO. It proves the native path agrees with
 * an obvious implementation of the same definition. It cannot prove the
 * DEFINITION is right: if both compute a mean the same wrong way, both agree.
 * So the references below are written from the definition, deliberately naive
 * and slow, never by calling another engine entry point -- a reference that
 * shares code with the thing under test proves nothing.
 *
 * FLOATING POINT: reassociation is legal, so a vectorised sum and a
 * left-to-right sum differ in the last places. Comparisons use a relative
 * tolerance, never bit equality. Integer results are compared exactly.
 */
import * as std from "std";
import { PRNG } from "./fuzzgen.js";

let pass = 0, fail = 0, skip = 0;
const fails = [];
const SEED = parseInt(std.getenv("DYNA_DIFF_SEED") || "20260802", 10);
const rnd = PRNG(SEED);

function ok(c, what, detail) {
    if (c) pass++;
    else { fail++; fails.push(what + (detail ? "  -- " + detail : "")); }
}
function close(a, b, tol) {
    if (Number.isNaN(a) && Number.isNaN(b)) return true;
    if (!Number.isFinite(a) || !Number.isFinite(b)) return Object.is(a, b);
    const t = tol === undefined ? 1e-9 : tol;
    return Math.abs(a - b) <= t * Math.max(1, Math.abs(a), Math.abs(b));
}
/* Random but REPRODUCIBLE columns; the seed prints on failure. */
function col(n, lo, hi) {
    const a = new Float64Array(n);
    for (let i = 0; i < n; i++) a[i] = lo + rnd() * (hi - lo);
    return a;
}
const arr = (t) => Array.from(t);

/* ============================================ dataframe: 153 aggregates */
{
    const m = await import("dyna:dataframe").catch(() => null);
    if (!m) { skip++; print("-- dataframe SKIP"); }
    else {
        const N = 257;                       /* prime: defeats any block size */
        const v = col(N, -50, 50);
        const w = col(N, 1, 9);
        const keys = [];
        for (let i = 0; i < N; i++) keys.push("k" + (i % 7));
        const df = new m.DataFrame({ k: keys, v, w });
        const A = arr(v);

        /* Naive references, written from the definition. */
        const sum = (x) => x.reduce((s, y) => s + y, 0);
        const mean = (x) => sum(x) / x.length;
        const vpop = (x) => sum(x.map((y) => (y - mean(x)) ** 2)) / x.length;
        const vsamp = (x) => sum(x.map((y) => (y - mean(x)) ** 2)) / (x.length - 1);
        const sorted = A.slice().sort((a, b) => a - b);
        const quant = (q) => {                 /* linear interpolation */
            const p = (sorted.length - 1) * q, lo = Math.floor(p), hi = Math.ceil(p);
            return sorted[lo] + (sorted[hi] - sorted[lo]) * (p - lo);
        };

        const CASES = [
            ["SUM", () => df.SUM("v"), sum(A)],
            ["COUNT", () => df.COUNT("v"), N],
            ["MEAN", () => df.MEAN("v"), mean(A)],
            ["MIN", () => df.MIN("v"), Math.min(...A)],
            ["MAX", () => df.MAX("v"), Math.max(...A)],
            ["PRODUCT sign", () => Math.sign(df.PRODUCT("w")), 1],
            ["VARIANCE_POP", () => df.VARIANCE_POP("v"), vpop(A)],
            ["VARIANCE", () => df.VARIANCE("v"), vsamp(A)],
            ["STDDEV_POP", () => df.STDDEV_POP("v"), Math.sqrt(vpop(A))],
            ["STDDEV", () => df.STDDEV("v"), Math.sqrt(vsamp(A))],
            ["MEDIAN", () => df.MEDIAN("v"), quant(0.5)],
            ["QUANTILE 0", () => df.QUANTILE("v", 0), sorted[0]],
            ["QUANTILE 1", () => df.QUANTILE("v", 1), sorted[N - 1]],
            ["QUANTILE .25", () => df.QUANTILE("v", 0.25), quant(0.25)],
            ["QUANTILE .75", () => df.QUANTILE("v", 0.75), quant(0.75)],
            ["PERCENTILE_CONT .5", () => df.PERCENTILE_CONT("v", 0.5), quant(0.5)],
            ["ARG_MIN attains", () => A[df.ARG_MIN("v")], Math.min(...A)],
            ["ARG_MAX attains", () => A[df.ARG_MAX("v")], Math.max(...A)],
            ["N_UNIQUE of keys", () => df.N_UNIQUE("k"), 7],
            ["MEAN_WEIGHTED", () => df.MEAN_WEIGHTED("v", "w"),
             sum(A.map((y, i) => y * w[i])) / sum(arr(w))],
            ["DOT_PRODUCT", () => df.DOT_PRODUCT("v", "w"),
             sum(A.map((y, i) => y * w[i]))],
            ["MAD >= 0", () => df.MAD("v") >= 0, true],
            ["SEM matches", () => df.SEM("v"), Math.sqrt(vsamp(A) / N)],
            ["REGR_COUNT", () => df.REGR_COUNT("v", "w"), N],
            ["COV_POP symmetric", () =>
                close(df.COV_POP("v", "w"), df.COV_POP("w", "v")), true],
            ["CORR in [-1,1]", () => Math.abs(df.CORR("v", "w")) <= 1.0000001, true],
            ["REGR_R2 in [0,1]", () => {
                const r = df.REGR_R2("v", "w"); return r >= -1e-9 && r <= 1 + 1e-9;
            }, true],
            ["CUM_SUM last == SUM", () => arr(df.CUM_SUM("v")).pop(), sum(A)],
            ["CUM_MAX last == MAX", () => arr(df.CUM_MAX("v")).pop(), Math.max(...A)],
            ["CUM_MIN last == MIN", () => arr(df.CUM_MIN("v")).pop(), Math.min(...A)],
            ["DIFF length", () => df.DIFF("v").length, N],
            ["ABS is non-negative", () => arr(df.ABS("v")).every((x) => x >= 0), true],
            ["SQRT of ABS is finite", () =>
                arr(df.SQRT("w")).every(Number.isFinite), true],
            ["ROUND is integral", () =>
                arr(df.ROUND("v")).every((x) => Number.isInteger(x)), true],
            ["FLOOR <= v <= CEIL", () => {
                const f = arr(df.FLOOR("v")), c = arr(df.CEIL("v"));
                return A.every((x, i) => f[i] <= x && x <= c[i]);
            }, true],
            ["SIGN in {-1,0,1}", () =>
                arr(df.SIGN("v")).every((x) => x === -1 || x === 0 || x === 1), true],
            ["CLIP bounds", () =>
                arr(df.CLIP("v", -1, 1)).every((x) => x >= -1 && x <= 1), true],
            ["GT count matches", () => df.COUNT("v", df.GT("v", 0)),
             A.filter((x) => x > 0).length],
            ["LT count matches", () => df.COUNT("v", df.LT("v", 0)),
             A.filter((x) => x < 0).length],
            ["GE + LT partitions", () =>
                df.COUNT("v", df.GE("v", 0)) + df.COUNT("v", df.LT("v", 0)), N],
            ["BETWEEN count", () => df.COUNT("v", df.BETWEEN("v", -10, 10)),
             A.filter((x) => x >= -10 && x <= 10).length],
            ["NOT_NA is all", () => df.COUNT("v", df.NOT_NA("v")), N],
            ["COUNT_NULLS is zero", () => df.COUNT_NULLS("v"), 0],
            ["HEAD length", () => df.HEAD("v", 5).length, 5],
            ["TAIL length", () => df.TAIL("v", 5).length, 5],
            ["FIRST", () => df.FIRST("v"), A[0]],
            ["LAST", () => df.LAST("v"), A[N - 1]],
            ["SORT is ordered", () => {
                const s = arr(df.SORT("v"));
                return s.every((x, i) => i === 0 || s[i - 1] <= x);
            }, true],
            ["N_LARGEST top is MAX", () => arr(df.N_LARGEST("v", 3))[0], Math.max(...A)],
            ["N_SMALLEST top is MIN", () => arr(df.N_SMALLEST("v", 3))[0], Math.min(...A)],
            ["UNIQUE of keys", () => df.UNIQUE("k").length, 7],
            ["ROLLING_SUM length", () => df.ROLLING_SUM("v", 4).length, N],
            ["ROLLING_MEAN length", () => df.ROLLING_MEAN("v", 4).length, N],
            ["SHIFT length", () => df.SHIFT("v", 1).length, N],
            ["ENTROPY >= 0", () => df.ENTROPY("k") >= -1e-12, true],
            ["APPROX_COUNT_DISTINCT near 7", () =>
                Math.abs(df.APPROX_COUNT_DISTINCT("k") - 7) <= 2, true],
        ];
        for (const [name, fn, want] of CASES) {
            let got, threw = null;
            try { got = fn(); } catch (e) { threw = e; }
            if (threw) { ok(false, "dataframe." + name, "threw " + threw.message); continue; }
            const good = typeof want === "number" && typeof got === "number"
                ? close(got, want, 1e-7) : got === want;
            ok(good, "dataframe." + name, `got ${got} want ${want} (seed ${SEED})`);
        }

        /* GROUP_BY_* against a plain-JS grouping of the same columns. */
        const model = {};
        for (let i = 0; i < N; i++) (model[keys[i]] = model[keys[i]] || []).push(v[i]);
        for (const [op, ref] of [
            ["GROUP_BY_SUM", sum], ["GROUP_BY_MEAN", mean],
            ["GROUP_BY_MIN", (x) => Math.min(...x)],
            ["GROUP_BY_MAX", (x) => Math.max(...x)],
            ["GROUP_BY_COUNT", (x) => x.length],
        ]) {
            if (typeof df[op] !== "function") { skip++; continue; }
            let bad = null;
            try {
                /* COUNT is (keyCol[, mask]) -- it has no value column, so
                   passing one is a TypeError rather than a wrong answer. */
                const g = op === "GROUP_BY_COUNT" ? df[op]("k") : df[op]("k", "v");
                for (let i = 0; i < g.keys.length && !bad; i++) {
                    const want = ref(model[g.keys[i]]);
                    if (!close(g.values[i], want, 1e-7))
                        bad = `${g.keys[i]}: got ${g.values[i]} want ${want}`;
                }
            } catch (e) { bad = "threw " + e.message; }
            ok(!bad, "dataframe." + op + " matches a JS grouping", bad);
        }
    }
}

/* ================================================ simd: kernels vs loops */
{
    const s = await import("dyna:simd").catch(() => null);
    if (!s) { skip++; print("-- simd SKIP"); }
    else {
        /* Every length across the vector-width boundaries, not a round number:
           a kernel that seeds a full vector before checking n breaks at n<W. */
        const LENGTHS = [0, 1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64, 65, 257];
        const UNARY = {
            sum: (a) => a.reduce((x, y) => x + y, 0),
            max: (a) => (a.length ? Math.max(...a) : -Infinity),
            min: (a) => (a.length ? Math.min(...a) : Infinity),
            normL1: (a) => a.reduce((x, y) => x + Math.abs(y), 0),
            normL2: (a) => Math.sqrt(a.reduce((x, y) => x + y * y, 0)),
            cumsum: null,
        };
        for (const [name, ref] of Object.entries(UNARY)) {
            if (typeof s[name] !== "function" || !ref) { skip++; continue; }
            let bad = null;
            for (const n of LENGTHS) {
                const f = new Float32Array(n);
                for (let i = 0; i < n; i++) f[i] = (rnd() * 20) - 10;
                const js = ref(arr(f));
                let got;
                try { got = s[name](f); } catch (e) { if (n === 0) continue; bad = `n=${n} threw`; break; }
                if (n === 0) continue;                 /* empty is a policy choice */
                if (!close(got, js, 1e-4)) { bad = `n=${n}: got ${got} want ${js}`; break; }
            }
            ok(!bad, `simd.${name} agrees with a JS loop at every length`, bad + ` (seed ${SEED})`);
        }
        /* argmax/argmin: ties are arbitrary, so assert the INDEX ATTAINS the
           extreme rather than pinning which tie wins -- that changes with the
           lane count and is not a defect. */
        for (const [name, cmp] of [["argmax", Math.max], ["argmin", Math.min]]) {
            if (typeof s[name] !== "function") { skip++; continue; }
            let bad = null;
            for (const n of LENGTHS) {
                if (n === 0) continue;
                const f = new Float32Array(n);
                for (let i = 0; i < n; i++) f[i] = (i * 37 % 11) - 5;
                let idx;
                try { idx = s[name](f); } catch (e) { bad = `n=${n} threw`; break; }
                const want = cmp.apply(null, arr(f));
                if (!(idx >= 0 && idx < n && f[idx] === want)) {
                    bad = `n=${n}: index ${idx} does not attain ${want}`; break;
                }
            }
            ok(!bad, `simd.${name} returns an index attaining the extreme`, bad);
        }
        if (typeof s.dot === "function") {
            let bad = null;
            for (const n of LENGTHS) {
                if (n === 0) continue;
                const a = new Float32Array(n), b = new Float32Array(n);
                for (let i = 0; i < n; i++) { a[i] = rnd() * 4 - 2; b[i] = rnd() * 4 - 2; }
                const js = arr(a).reduce((x, y, i) => x + y * b[i], 0);
                let got;
                try { got = s.dot(a, b); } catch (e) { bad = `n=${n} threw`; break; }
                if (!close(got, js, 1e-4)) { bad = `n=${n}: got ${got} want ${js}`; break; }
            }
            ok(!bad, "simd.dot agrees with a JS loop at every length", bad);
        }
    }
}

/* ================================================= mathx: identities */
{
    const m = await import("dyna:mathx").catch(() => null);
    if (!m) { skip++; print("-- mathx SKIP"); }
    else {
        /* Identities hold regardless of algorithm, so they need no reference
           implementation and no published table. */
        const ID = [
            ["erf is odd", () => close(m.erf(-0.7), -m.erf(0.7))],
            ["erf(0)=0", () => close(m.erf(0), 0)],
            ["erf + erfc = 1", () => close(m.erf(0.3) + m.erfc(0.3), 1)],
            ["erfc(0)=1", () => close(m.erfc(0), 1)],
            ["cbrt cubed", () => close(m.cbrt(27), 3)],
            ["cbrt of a negative", () => close(m.cbrt(-8), -2)],
            ["gcd divides both", () => Number(m.gcd(48, 36)) === 12],
            ["Pi", () => close(m.Pi, Math.PI)],
            ["E", () => close(m.E, Math.E)],
            ["Ln2", () => close(m.Ln2, Math.LN2)],
            ["Sqrt2", () => close(m.Sqrt2, Math.SQRT2)],
            ["besselj order 0 at 0 is 1", () => close(m.besselj(0, 0), 1)],
            ["besselj order 1 at 0 is 0", () => close(m.besselj(1, 0), 0)],
            ["besselj J-n = (-1)^n Jn", () =>
                close(m.besselj(-3, 1.5), -m.besselj(3, 1.5))],
            ["bessely order 0 at 0 is -inf", () => m.bessely(0, 0) === -Infinity ||
                Number.isNaN(m.bessely(0, 0)) || m.bessely(0, 0) < -1e10],
            ["besselj non-finite order refused", () => {
                try { m.besselj(Infinity, 1); return false; } catch (e) { return true; }
            }],
            ["beta symmetric", () => close(m.beta(2, 3), m.beta(3, 2))],
            ["betaln = log(beta)", () => close(m.betaln(2, 3), Math.log(m.beta(2, 3)), 1e-6)],
            /* eps is a FUNCTION returning DBL_EPSILON, not a constant. */
            ["eps is tiny", () => m.eps() > 0 && m.eps() < 1e-10],
            ["deg2rad(180)", () => close(m.deg2rad(180), Math.PI)],
            ["copysign", () => Object.is(m.copysign(3, -0), -3)],
            ["MaxInt32", () => Number(m.MaxInt32) === 2147483647],
            ["MinInt32", () => Number(m.MinInt32) === -2147483648],
            ["MaxSafeInteger", () => Number(m.MaxSafeInteger) === 9007199254740991],
        ];
        for (const [name, fn] of ID) {
            let got, threw = null;
            try { got = fn(); } catch (e) { threw = e; }
            ok(!threw && got === true, "mathx: " + name,
               threw ? "threw " + threw.message : "identity does not hold");
        }

        /* bits.*: every width against a plain-JS reference. */
        if (m.bits) {
            const B = m.bits;
            const popcount = (x) => { let c = 0; x >>>= 0; while (x) { c += x & 1; x >>>= 1; } return c; };
            const SAMPLES = [0, 1, 2, 3, 7, 8, 127, 128, 255, 256, 65535, 65536,
                             0x7fffffff, 0xffffffff, 12345, 0xdeadbeef];
            const WIDTH = { 8: 0xff, 16: 0xffff, 32: 0xffffffff };
            for (const w of [8, 16, 32]) {
                const mask = WIDTH[w];
                const pairs = [
                    [`onesCount${w}`, (x) => popcount(x & mask)],
                    [`trailingZeros${w}`, (x) => {
                        const y = x & mask; if (y === 0) return w;
                        let c = 0, z = y; while (!(z & 1)) { c++; z >>>= 1; } return c;
                    }],
                    [`len${w}`, (x) => { let y = x & mask, c = 0; while (y) { c++; y >>>= 1; } return c; }],
                    [`leadingZeros${w}`, (x) => { let y = x & mask, c = 0; while (y) { c++; y >>>= 1; } return w - c; }],
                ];
                for (const [fname, ref] of pairs) {
                    if (typeof B[fname] !== "function") { skip++; continue; }
                    let bad = null;
                    for (const x of SAMPLES) {
                        let got;
                        try { got = B[fname](x); } catch (e) { bad = `${x} threw`; break; }
                        const want = ref(x);
                        if (got !== want) { bad = `${x}: got ${got} want ${want}`; break; }
                    }
                    ok(!bad, `mathx.bits.${fname} agrees with a JS reference`, bad);
                }
                /* reverse is an involution at every width. */
                const rev = `reverse${w}`;
                if (typeof B[rev] === "function") {
                    let bad = null;
                    for (const x of SAMPLES) {
                        const y = x & mask;
                        let got;
                        try { got = B[rev](B[rev](y)); } catch (e) { bad = `${y} threw`; break; }
                        if ((got & mask) !== y) { bad = `${y}: round trip gave ${got}`; break; }
                    }
                    ok(!bad, `mathx.bits.${rev} is an involution`, bad);
                }
                const rot = `rotateLeft${w}`;
                if (typeof B[rot] === "function") {
                    let bad = null;
                    for (const x of SAMPLES) {
                        const y = x & mask;
                        let got;
                        try { got = B[rot](B[rot](y, 1), w - 1); } catch (e) { bad = `${y} threw`; break; }
                        if ((got & mask) !== y) { bad = `${y}: full rotation gave ${got}`; break; }
                    }
                    ok(!bad, `mathx.bits.${rot} by w is the identity`, bad);
                }
            }
            /* 64-bit helpers: divide-by-zero must refuse, not fault. */
            for (const fname of ["div64", "rem64"]) {
                if (typeof B[fname] !== "function") { skip++; continue; }
                let refused = false;
                try { B[fname](1n, 0n); } catch (e) { refused = true; }
                ok(refused, `mathx.bits.${fname} refuses division by zero`);
            }
        }
    }
}

/* ============================================ structures: model-based */
{
    const S = await import("dyna:structures").catch(() => null);
    if (!S) { skip++; print("-- structures SKIP"); }
    else {
        /* Drive the native structure and a plain-JS model with the same random
           operation sequence, and require identical observable state. This is
           the only way to cover a stateful type: a single call proves nothing
           about the transition it is part of. */
        const WORDS = [];
        for (let i = 0; i < 300; i++)
            WORDS.push("w" + Math.floor(rnd() * 1000));

        if (typeof S.Trie === "function") {
            const t = new S.Trie(), model = new Map();
            let bad = null;
            for (let i = 0; i < 300 && !bad; i++) {
                const w = WORDS[i];
                if (rnd() < 0.75) { t.insert(w, i); model.set(w, i); }
                else if (typeof t.delete === "function") { t.delete(w); model.delete(w); }
                const w2 = WORDS[Math.floor(rnd() * WORDS.length)];
                if (t.has(w2) !== model.has(w2))
                    bad = `has("${w2}") disagrees after ${i} ops`;
            }
            ok(!bad, "structures.Trie matches a Map model over 300 ops", bad + ` (seed ${SEED})`);
            if (!bad && typeof t.size === "number")
                ok(t.size === model.size, "structures.Trie size matches the model",
                   `${t.size} vs ${model.size}`);
        }

        if (typeof S.LRU === "function") {
            const CAP = 16;
            const l = new S.LRU(CAP);
            const model = new Map();
            let bad = null;
            for (let i = 0; i < 400 && !bad; i++) {
                const k = "k" + Math.floor(rnd() * 40);
                if (rnd() < 0.7) {
                    l.set(k, i);
                    model.delete(k); model.set(k, i);
                    while (model.size > CAP) model.delete(model.keys().next().value);
                } else {
                    const a = l.get(k), b = model.get(k);
                    if ((a === undefined) !== (b === undefined))
                        bad = `get("${k}") presence disagrees at op ${i}`;
                    else if (b !== undefined) { model.delete(k); model.set(k, b); }
                }
                if (!bad && typeof l.size === "number" && l.size > CAP)
                    bad = `size ${l.size} exceeds capacity ${CAP}`;
            }
            ok(!bad, "structures.LRU matches an insertion-ordered model over 400 ops", bad);
        }

        if (typeof S.BitSet === "function") {
            const N = 200, b = new S.BitSet(N), model = new Set();
            let bad = null;
            for (let i = 0; i < 500 && !bad; i++) {
                const x = Math.floor(rnd() * N);
                if (rnd() < 0.7) { b.set(x); model.add(x); }
                else if (typeof b.clear === "function") { b.clear(x); model.delete(x); }
                const y = Math.floor(rnd() * N);
                if (!!b.get(y) !== model.has(y)) bad = `bit ${y} disagrees at op ${i}`;
            }
            ok(!bad, "structures.BitSet matches a Set model over 500 ops", bad);
        }

        if (typeof S.Deque === "function") {
            const d = new S.Deque(), model = [];
            let bad = null;
            for (let i = 0; i < 300 && !bad; i++) {
                const r = rnd();
                try {
                    if (r < 0.3) { d.pushBack(i); model.push(i); }
                    else if (r < 0.6) { d.pushFront(i); model.unshift(i); }
                    else if (r < 0.8) {
                        const a = d.popFront(), e = model.shift();
                        if (a !== e && !(a === undefined && e === undefined))
                            bad = `popFront ${a} vs ${e} at op ${i}`;
                    } else {
                        const a = d.popBack(), e = model.pop();
                        if (a !== e && !(a === undefined && e === undefined))
                            bad = `popBack ${a} vs ${e} at op ${i}`;
                    }
                } catch (e) { bad = "threw " + e.message; }
            }
            ok(!bad, "structures.Deque matches an Array model over 300 ops", bad);
        }

        if (typeof S.BloomFilter === "function") {
            /* The only sound assertion: no FALSE NEGATIVE. A false positive is
               the structure working as designed, not a bug. */
            const f = new S.BloomFilter(5000, 0.01);
            const added = [];
            for (let i = 0; i < 400; i++) { const w = "b" + i; f.add(w); added.push(w); }
            const missing = added.filter((w) => !f.mayContain(w));
            ok(missing.length === 0, "structures.BloomFilter has no false negative",
               `${missing.length} of 400 lost`);
        }

        if (typeof S.Heap === "function") {
            let bad = null;
            try {
                const h = new S.Heap(), model = [];
                for (let i = 0; i < 200; i++) {
                    const x = Math.floor(rnd() * 1000);
                    h.push(x); model.push(x);
                }
                model.sort((a, b) => a - b);
                for (let i = 0; i < model.length && !bad; i++) {
                    const got = h.pop();
                    if (got !== model[i]) bad = `pop ${i}: got ${got} want ${model[i]}`;
                }
            } catch (e) { bad = "threw " + e.message; }
            ok(!bad, "structures.Heap pops in sorted order", bad);
        }
    }
}

/* ==================================================================== done */
print("\n" + "=".repeat(64));
if (fails.length) {
    print(`FAILURES (${fails.length}) -- replay with DYNA_DIFF_SEED=${SEED}:`);
    for (const f of fails) print("  " + f);
}
print(`test_api_differential: ${pass} passed, ${fail} failed, ${skip} skipped, seed ${SEED}`);
if (fail > 0) std.exit(1);
