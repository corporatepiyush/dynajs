/* dyna:dataframe vs the JS loops it replaces.
 *
 * The comparison that matters is NOT "native beats JS" -- that is assumed. It is
 * whether a masked/grouped primitive gets close to the memory-bound floor, which
 * is what `Float64Array.sum()` (already native, already SIMD) measures. A
 * primitive that lands near the floor has nothing left to give; one that does
 * not still has a real inefficiency.
 *
 * Two sections, selected by argv so an A/B does not pay for the one it is not
 * reading:
 *   `bench_dataframe.js agg`     the per-aggregate ns/element matrix at 100 /
 *                               10k / 1M rows -- machine-readable ROW lines,
 *                               diffable between two binaries.
 *   `bench_dataframe.js legacy`  the original vs-JS-loop story at 2M rows.
 * With no argument both run.
 *
 * The matrix exists because a reduction has TWO regimes and one number hides
 * that: a wide body amortises over a megabyte and is pure overhead over a
 * hundred elements. Every row is therefore measured at three sizes, and rows
 * tagged CONTROL touch code no aggregate change may reach -- if a control moves,
 * the run is invalid and nothing else in it can be believed.
 *
 * FOUR WAYS A BENCHMARK LIES TO ITSELF, and what is done about each here:
 *
 *  (a) A REDUCTION WHOSE RESULT IS UNUSED IS DEAD CODE. Every timed closure
 *      returns a Number and every Number is folded into SINK, which is printed.
 *      A closure whose result is dropped can be optimised away wholesale and the
 *      row then measures the empty loop.
 *  (b) CSE ACROSS REPETITIONS. Every scalar operand is read from KS[] through a
 *      counter that advances per call, so the argument is not the same value
 *      twice running and no repetition can be folded into the previous one. The
 *      values are chosen to cost the same as each other, so this defeats reuse
 *      without changing the work.
 *  (c) A SCALAR OPERAND THAT GETS CONSTANT-FOLDED. This one has already cost us
 *      a wrong number: pow read 0.09 ns/elem when the real cost was 7.8, because
 *      an INTEGRAL exponent takes libm's fast path and is not a pow at all. Both
 *      rows are therefore reported -- pow.k2 (exponent 2) and pow.frac (exponent
 *      2.7) -- and the gap between them is the evidence that the guard is
 *      needed. Never quote pow.k2 as the cost of pow.
 *  (d) THE TIMED REGION CONTAINING ANYTHING BUT THE OPERATION. Two calibrations
 *      are measured and subtracted, not one: `noop` for rows whose closure
 *      returns a bare number, and `noopArr` for rows that must read `.length`
 *      off a returned TypedArray, so that property load is not billed to the
 *      kernel.
 *
 * EARLY-EXIT OPS ARE MEASURED ON THEIR ADVERSARIAL INPUT. `any` over a mask with
 * an early hit and `all` over a mask with an early miss both answer after k
 * elements while the row divides by n, and the ns/element that comes out is a
 * lie about a kernel that never ran. The rows that count are any.allFalse and
 * all.allTrue -- the shapes where the bypass never fires and every byte is
 * touched. The friendly shapes are reported too, marked, for the contrast.
 */
import { DataFrame } from "dyna:dataframe";

const ARGS = (typeof scriptArgs === "object" && scriptArgs) ? scriptArgs : [];
const WANT = ARGS.filter((a) => a === "agg" || a === "legacy");
const RUN_AGG = WANT.length === 0 || WANT.indexOf("agg") >= 0;
const RUN_LEGACY = WANT.length === 0 || WANT.indexOf("legacy") >= 0;

/* ======================================================================
   [A] aggregate matrix -- ns/element at three sizes
   ====================================================================== */
if (RUN_AGG) {
    let SINK = 0;

    /* guard (b): a rotating scalar operand. Every entry costs the same to
       compute with, so rotating changes nothing but the ability to reuse. */
    const KS = [1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5, 8.5];
    let kc = 0;
    const K = () => KS[(kc++) & 7];

    function nsPerCall(f, reps) {
        for (let r = 0; r < (reps < 50 ? reps : 50); r++) SINK += f();   /* warm */
        let best = Infinity;
        for (let t = 0; t < 3; t++) {
            const t0 = performance.now();
            for (let r = 0; r < reps; r++) SINK += f();
            const d = (performance.now() - t0) * 1e6 / reps;
            if (d < best) best = d;
        }
        return best;
    }

    /* guard (d): two calibrations. The driving loop itself costs several ns per
       iteration, and a row that reads `.length` off a fresh TypedArray pays a
       property load the kernel did not. Billing that to the kernel is worth
       0.02-0.2 ns/elem at n=100, which is a visible fraction of the row. */
    let ctr = 0;
    const noop = () => ++ctr;
    const CALARR = new Float64Array(8);
    const noopArr = () => CALARR.length;

    const SIZES = [100, 10000, 1000000];
    const rows = [];

    function build(n) {
        const f64 = new Float64Array(n);
        const f64b = new Float64Array(n);
        const f64pos = new Float64Array(n);        /* > 0, so log/sqrt are real work */
        const i32 = new Int32Array(n);
        const u8 = new Uint8Array(n);
        const key = new Int32Array(n);
        for (let i = 0; i < n; i++) {
            f64[i] = (i % 1000) * 0.5 + 1;
            f64b[i] = (i % 37) * 0.25 + 1;
            f64pos[i] = (i % 97) + 1;
            i32[i] = (i * 2654435761) | 0;
            u8[i] = i & 255;
            key[i] = i % 64;
        }
        const df = new DataFrame({ f64, f64b, f64pos, i32, u8, key });
        /* ~50% selectivity: a mask that is all-ones or all-zeros measures a
           branch predictor, not a kernel. */
        const mask = df.gt("f64", 250);
        return {
            n, df, mask, f64,
            allOnes: new Uint8Array(n).fill(1),
            allZero: new Uint8Array(n),
            /* the FRIENDLY shapes for all/any: the bypass fires immediately and
               the row divides k by n. Reported, never quoted as the cost. */
            firstTrue: (() => { const u = new Uint8Array(n); if (n) u[0] = 1; return u; })(),
            firstFalse: (() => { const u = new Uint8Array(n).fill(1); if (n) u[0] = 0; return u; })(),
        };
    }

    /* `arrRow` says the closure reads `.length` and must be calibrated against
       noopArr instead of noop. Same output format either way. */
    function row(name, n, f, reps, arrRow) {
        const raw = nsPerCall(f, reps);
        const cal = nsPerCall(arrRow ? noopArr : noop, reps);
        const per = (raw - cal) / n;
        rows.push({ name, n, per, call: raw - cal });
        console.log("ROW " + name.padEnd(24) + String(n).padStart(8) +
                    per.toFixed(4).padStart(11) + " ns/elem " +
                    (raw - cal).toFixed(1).padStart(12) + " ns/call");
    }

    console.log("# ROW <name> <rows> <ns/elem> <ns/call>   (min of 3, empty loop subtracted)");
    for (const n of SIZES) {
        const c = build(n);
        const df = c.df, mask = c.mask, ones = c.allOnes, zero = c.allZero;
        /* enough reps that the timed region clears the clock's noise floor at
           every size, without making 1M rows take a minute */
        const reps = Math.max(30, Math.ceil(3e7 / n));
        /* the transcendentals are 20-100x the cost of an add per element; at 1M
           the same rep count would make this section take minutes */
        const slow = Math.max(5, reps >> 4);
        const A = true;                      /* shorthand: this row reads .length */

        /* ---- reductions, the family the accumulator count changes ---- */
        row("sum.f64", n, () => df.sum("f64"), reps);
        row("sum.f64.masked", n, () => df.sum("f64", mask), reps);
        row("sum.i32", n, () => df.sum("i32"), reps);
        row("sum.u8", n, () => df.sum("u8"), reps);
        row("mean.f64", n, () => df.mean("f64"), reps);
        row("min.f64", n, () => df.min("f64"), reps);
        row("max.f64", n, () => df.max("f64"), reps);
        row("min.i32", n, () => df.min("i32"), reps);
        row("max.i32", n, () => df.max("i32"), reps);
        row("min.f64.masked", n, () => df.min("f64", mask), reps);
        row("count.masked", n, () => df.count("f64", mask), reps);
        row("product.f64", n, () => df.product("f64"), reps);
        row("product.i32", n, () => df.product("i32"), reps);
        row("product.f64.masked", n, () => df.product("f64", mask), reps);
        row("dot.f64", n, () => df.dotProduct("f64", "f64b"), reps);
        row("dot.f64.masked", n, () => df.dotProduct("f64", "f64b", mask), reps);
        row("dot.i32", n, () => df.dotProduct("i32", "i32"), reps);
        /* the mixed narrow pair is the GENERIC block-widening path, not one of
           the fifteen specialised kernels -- a different cost entirely */
        row("dot.u8xi32.generic", n, () => df.dotProduct("u8", "i32"), reps);
        row("variance.f64", n, () => df.variance("f64"), reps);
        row("variance.f64.masked", n, () => df.variance("f64", mask), reps);
        row("stddev.f64", n, () => df.stddev("f64"), reps);
        row("variance.i32", n, () => df.variance("i32"), reps);
        row("bitwiseAnd.i32", n, () => df.bitwiseAnd("i32"), reps);
        row("bitwiseOr.i32", n, () => df.bitwiseOr("i32"), reps);
        row("bitwiseXor.i32", n, () => df.bitwiseXor("i32"), reps);
        row("bitwiseXor.u8", n, () => df.bitwiseXor("u8"), reps);
        row("bitwiseAnd.masked", n, () => df.bitwiseAnd("i32", mask), reps);

        /* ---- all/any: ADVERSARIAL first, and it is the number that counts ----
           On these two shapes the bypass never fires, every byte is read, and
           the denominator n is the work actually done. */
        row("ADV.all.allTrue", n, () => (df.all(ones) ? 1 : 0), reps);
        row("ADV.any.allFalse", n, () => (df.any(zero) ? 1 : 0), reps);
        row("ADV.allScan.allTrue", n, () => (df.allScan(ones) ? 1 : 0), reps);
        row("ADV.anyScan.allFalse", n, () => (df.anyScan(zero) ? 1 : 0), reps);
        row("ADV.allEarly.allTrue", n, () => (df.allEarly(ones) ? 1 : 0), reps);
        row("ADV.anyEarly.allFalse", n, () => (df.anyEarly(zero) ? 1 : 0), reps);
        /* the friendly shapes: the bypass fires at element 0, so ns/elem here is
           a per-CALL cost divided by n and means nothing about the kernel */
        row("easy.any.firstTrue", n, () => (df.any(c.firstTrue) ? 1 : 0), reps);
        row("easy.all.firstFalse", n, () => (df.all(c.firstFalse) ? 1 : 0), reps);
        row("easy.anyEarly.first", n, () => (df.anyEarly(c.firstTrue) ? 1 : 0), reps);
        row("any.mixed", n, () => (df.any(mask) ? 1 : 0), reps);

        /* ---- producers: these return a TypedArray, so calibrate on noopArr ---- */
        row("bitmask", n, () => df.bitmask(mask).length, reps, A);
        row("abs.f64", n, () => df.abs("f64").length, reps, A);
        row("abs.i32", n, () => df.abs("i32").length, reps, A);
        row("round.f64", n, () => df.round("f64").length, reps, A);
        row("floor.f64", n, () => df.floor("f64").length, reps, A);
        row("ceil.f64", n, () => df.ceil("f64").length, reps, A);
        row("sign.f64", n, () => df.sign("f64").length, reps, A);
        row("sqrt.f64", n, () => df.sqrt("f64pos").length, slow, A);
        row("log.f64", n, () => df.log("f64pos").length, slow, A);
        row("exp.f64", n, () => df.exp("f64b").length, slow, A);
        row("isna.f64", n, () => df.isna("f64").length, reps, A);
        row("notna.f64", n, () => df.notna("f64").length, reps, A);
        row("isna.i32", n, () => df.isna("i32").length, reps, A);
        row("between.f64", n, () => df.between("f64", K(), 400).length, reps, A);
        row("clip.f64", n, () => df.clip("f64", K(), 400).length, reps, A);
        row("fillna.f64", n, () => df.fillna("f64", K()).length, reps, A);
        row("add.col", n, () => df.add("f64", "f64b").length, reps, A);
        row("add.scalar", n, () => df.add("f64", K()).length, reps, A);
        row("sub.col", n, () => df.sub("f64", "f64b").length, reps, A);
        row("mul.col", n, () => df.mul("f64", "f64b").length, reps, A);
        row("div.col", n, () => df.div("f64", "f64b").length, reps, A);
        row("rsub.scalar", n, () => df.rsub("f64", K()).length, reps, A);
        row("rdiv.scalar", n, () => df.rdiv("f64", K()).length, reps, A);
        row("add.widen.i32", n, () => df.add("i32", "f64b").length, reps, A);
        /* guard (c) in the output, not just the comment: an INTEGRAL exponent
           takes libm's fast path. If these two rows are within 2x of each other
           the guard has stopped working and pow.frac is no longer a pow. */
        row("pow.k2", n, () => df.pow("f64", 2).length, slow, A);
        row("pow.frac", n, () => df.pow("f64", 2.7).length, slow, A);
        row("pow.col", n, () => df.pow("f64b", "f64b").length, slow, A);
        row("where.cc", n, () => df.where(mask, "f64", "f64b").length, reps, A);
        row("where.cs", n, () => df.where(mask, "f64", K()).length, reps, A);
        row("where.ss", n, () => df.where(mask, K(), 0).length, reps, A);
        row("eq.f64", n, () => df.eq("f64", K()).length, reps, A);
        row("ne.f64", n, () => df.ne("f64", K()).length, reps, A);
        row("le.f64", n, () => df.le("f64", K()).length, reps, A);
        row("ge.f64", n, () => df.ge("f64", K()).length, reps, A);
        row("lt.i32", n, () => df.lt("i32", 0).length, reps, A);

        /* ---- CONTROLS ----
           Nothing in the reduction kernel family reaches any of these, so all
           four must sit still across an A/B of an accumulator-count or
           reduction-macro change. If one moves, the BINARY moved -- code layout,
           a different compiler, another build running -- and no other row in the
           run can be believed. gt is the compare path; groupBySum is the
           scatter; abs is the map1 table; where is the branchless select. The
           last two were added because gt and groupBySum are both integer-keyed
           and would miss a float-side layout effect between them. */
        row("CONTROL.gt", n, () => df.gt("f64", 250).length, reps, A);
        row("CONTROL.groupBySum", n,
            () => df.groupBySum("key", "f64").values[0], Math.max(20, reps >> 3));
        row("CONTROL.abs.map", n, () => df.abs("f64b").length, reps, A);
        row("CONTROL.where.sel", n, () => df.where(ones, "f64", "f64b").length, reps, A);
    }
    console.log("# sink " + (SINK === 12345 ? "?" : "ok"));

    /* guard (c), asserted rather than assumed */
    {
        const k2 = rows.find((r) => r.name === "pow.k2" && r.n === 1000000);
        const kf = rows.find((r) => r.name === "pow.frac" && r.n === 1000000);
        if (k2 && kf)
            console.log("# pow guard: integral exponent " + k2.per.toFixed(4) +
                        " vs fractional " + kf.per.toFixed(4) + " ns/elem, ratio " +
                        (kf.per / k2.per).toFixed(1) + "x" +
                        (kf.per / k2.per < 2 ? "   <-- GUARD STOPPED WORKING, pow.frac is not a pow"
                                             : "   (the fast path is real; quote pow.frac)"));
    }

    /* Differential: every aggregate against the obviously-correct JS form.
       A fast wrong answer is worthless, and a reduction that silently reads one
       element too few looks exactly like a speedup. */
    {
        const n = 4096;
        const x = new Float64Array(n), y = new Float64Array(n);
        const iv = new Int32Array(n), u = new Uint8Array(n);
        for (let i = 0; i < n; i++) {
            x[i] = Math.sin(i) * 100 + 200;
            y[i] = Math.cos(i) * 3 + 4;
            iv[i] = (i * 2654435761) | 0;
            u[i] = i & 255;
        }
        /* A product over 4096 values averaging 4 overflows to +Infinity, and
           comparing two infinities proves nothing -- an implementation that
           returned Infinity for everything would pass. `pv` alternates 2 and
           0.5 so every partial product is exactly 1 or 2 and the differential
           can still fail. (This caught the reference, not the kernel: the first
           version of this block reported a MISMATCH of rel NaN, which is what
           |Inf - Inf| / Inf evaluates to.) */
        const pv = new Float64Array(n);
        for (let i = 0; i < n; i++) pv[i] = (i & 1) ? 0.5 : 2;
        const df = new DataFrame({ x, y, iv, u, pv });
        const m = df.gt("x", 200);
        const rel = (a, b) => (Object.is(a, b) ? 0
                               : Math.abs(a - b) / Math.max(1e-300, Math.abs(b)));
        let bad = 0;
        const say = (w, r) => {
            if (!(r < 1e-12)) bad++;
            console.log("  " + w.padEnd(18) + (r < 1e-12 ? "ok" : "MISMATCH") +
                        "  rel " + r.toExponential(2));
        };
        console.log("\n[A9] aggregates vs the JS loops");
        {
            let d = 0; for (let i = 0; i < n; i++) d += x[i] * y[i];
            say("dotProduct", rel(df.dotProduct("x", "y"), d));
            let dm = 0; for (let i = 0; i < n; i++) if (m[i]) dm += x[i] * y[i];
            say("dotProduct mask", rel(df.dotProduct("x", "y", m), dm));
            let dg = 0; for (let i = 0; i < n; i++) dg += u[i] * iv[i];
            say("dot generic", rel(df.dotProduct("u", "iv"), dg));
        }
        {
            let s = 0; for (let i = 0; i < n; i++) s += x[i];
            const mu = s / n;
            let v = 0; for (let i = 0; i < n; i++) v += (x[i] - mu) * (x[i] - mu);
            say("variance", rel(df.variance("x"), v / (n - 1)));
            say("stddev", rel(df.stddev("x"), Math.sqrt(v / (n - 1))));
        }
        {
            let p = 1; for (let i = 0; i < n; i++) p *= pv[i];
            say("product", rel(df.product("pv"), p));
            if (!Number.isFinite(p))
                console.log("  ** the product reference overflowed; this row is not a test **");
            let a = -1, o = 0, xr = 0;
            for (let i = 0; i < n; i++) { a &= iv[i]; o |= iv[i]; xr ^= iv[i]; }
            say("bitwiseAnd", rel(df.bitwiseAnd("iv"), a));
            say("bitwiseOr", rel(df.bitwiseOr("iv"), o));
            say("bitwiseXor", rel(df.bitwiseXor("iv"), xr));
            let su = 0; for (let i = 0; i < n; i++) su += u[i];
            say("sum.u8", rel(df.sum("u"), su));
        }
        {
            const bm = df.bitmask(m);
            let b2 = 0;
            for (let i = 0; i < n; i++)
                if (((bm[i >> 5] >>> (i & 31)) & 1) !== (m[i] ? 1 : 0)) b2++;
            if (b2) bad++;
            console.log("  " + "bitmask".padEnd(18) + (b2 ? "MISMATCH " + b2 : "ok"));
        }
        {   /* the maps claim BIT-IDENTITY with the JS loop, not a tolerance */
            const chk = (name, got, f) => {
                let w = -1;
                for (let i = 0; i < n; i++) {
                    const want = f(x[i]);
                    if (!Object.is(got[i], want) && !(Number.isNaN(got[i]) && Number.isNaN(want))) { w = i; break; }
                }
                if (w >= 0) bad++;
                console.log("  " + name.padEnd(18) + (w < 0 ? "ok (bit-identical)" : "MISMATCH at " + w));
            };
            chk("abs", df.abs("x"), Math.abs);
            chk("round", df.round("x"), Math.round);
            chk("sqrt", df.sqrt("x"), Math.sqrt);
            chk("log", df.log("x"), Math.log);
        }
        console.log(bad ? "  ** " + bad + " DIFFERENTIAL MISMATCH(ES): the numbers above are not "
                          + "measuring a correct kernel **"
                        : "  all differentials clean");
    }
}

/* ======================================================================
   [B] the original vs-JS-loop story at 2M rows
   ====================================================================== */
if (RUN_LEGACY) {

function bench(name, f) {
    for (let i = 0; i < 2; i++) f();
    let best = Infinity;
    for (let r = 0; r < 5; r++) {
        const t0 = performance.now(); f(); const t1 = performance.now();
        if (t1 - t0 < best) best = t1 - t0;
    }
    console.log("  " + name.padEnd(38) + best.toFixed(3).padStart(9) + " ms");
    return best;
}

const N = 2000000;
const NGROUPS = 64;

const price = new Float64Array(N);
const qty = new Int32Array(N);
const keyc = new Int32Array(N);
const city = [];
const CITIES = [];
for (let i = 0; i < NGROUPS; i++) CITIES.push("city" + i);
for (let i = 0; i < N; i++) {
    price[i] = (i % 1000) * 0.5;
    qty[i] = i % 97;
    keyc[i] = i % NGROUPS;
}
/* the string column is dictionary-encoded at construction; build it once */
for (let i = 0; i < N; i++) city.push(CITIES[i % NGROUPS]);

console.log("rows = " + N + ", groups = " + NGROUPS);

console.log("\n[1] unmasked sum -- vs the memory-bound floor");
const floor = bench("Float64Array.sum()  (floor)", () => price.sum());
const jsSum = bench("JS for-loop sum", () => { let s = 0; for (let i = 0; i < N; i++) s += price[i]; return s; });
const t0 = performance.now();
const df = new DataFrame({ price, qty, keyc, city });
const ctorMs = performance.now() - t0;
const dfSum = bench("df.sum('price')", () => df.sum("price"));
console.log("  -> vs JS loop " + (jsSum / dfSum).toFixed(1) + "x, " +
            "vs floor " + (dfSum / floor).toFixed(2) + "x");
console.log("  (DataFrame construction, incl. dictionary-encoding " + N +
            " strings: " + ctorMs.toFixed(1) + " ms, one time)");

console.log("\n[2] predicate -> mask");
const jsFilter = bench("JS filter count (price>250)", () => { let c = 0; for (let i = 0; i < N; i++) if (price[i] > 250) c++; return c; });
const dfMask = bench("df.gt('price',250)", () => df.gt("price", 250));
console.log("  -> " + (jsFilter / dfMask).toFixed(1) + "x");

console.log("\n[3] MASKED reduction -- no engine primitive exists for this");
const mask = df.gt("price", 250);
const jsMasked = bench("JS masked sum", () => { let s = 0; for (let i = 0; i < N; i++) if (price[i] > 250) s += price[i]; return s; });
const dfMasked = bench("df.sum('price', mask)", () => df.sum("price", mask));
console.log("  -> " + (jsMasked / dfMasked).toFixed(1) + "x");

console.log("\n[4] GROUP-BY sum -- no engine primitive exists for this");
const jsGroup = bench("JS groupby-sum (int keys)", () => {
    const g = new Float64Array(NGROUPS);
    for (let i = 0; i < N; i++) g[keyc[i]] += price[i];
    return g[0];
});
const dfGroupI = bench("df.groupBySum('keyc','price')", () => df.groupBySum("keyc", "price"));
const dfGroupS = bench("df.groupBySum('city','price')", () => df.groupBySum("city", "price"));
console.log("  -> int keys " + (jsGroup / dfGroupI).toFixed(1) + "x, " +
            "string keys " + (jsGroup / dfGroupS).toFixed(1) +
            "x (a JS string-keyed groupby with a Map is far slower still)");

console.log("\n[5] the same pipeline end to end (filter -> group -> sum)");
const jsPipe = bench("JS: filter+groupby", () => {
    const g = new Float64Array(NGROUPS);
    for (let i = 0; i < N; i++) if (price[i] > 250) g[keyc[i]] += price[i];
    return g[0];
});
const dfPipe = bench("df: gt + groupBySum(mask)", () => {
    const m = df.gt("price", 250);
    return df.groupBySum("keyc", "price", m);
});
console.log("  -> " + (jsPipe / dfPipe).toFixed(1) + "x");

console.log("\n[6] element-wise pipeline -- the map/where family, not a reduction");
const jsMap = bench("JS: (price*1.2 + 3) clipped", () => {
    const o = new Float64Array(N);
    for (let i = 0; i < N; i++) {
        const v = price[i] * 1.2 + 3;
        o[i] = v < 10 ? 10 : (v > 400 ? 400 : v);
    }
    return o[0];
});
const dfMap = bench("df: mul -> add -> clip", () => {
    const a = new DataFrame({ v: df.mul("price", 1.2) });
    const b = new DataFrame({ v: a.add("v", 3) });
    return b.clip("v", 10, 400)[0];
});
console.log("  -> " + (jsMap / dfMap).toFixed(1) +
            "x (three passes and two intermediate frames against one fused JS loop:" +
            " the losing shape, reported because it is the one users write)");

/* Correctness is not optional in a benchmark: a fast wrong answer is worthless,
   and a silently-wrong kernel is exactly the failure mode SIMD code has. */
console.log("\n[7] results agree with the JS loops");
let ref = 0; for (let i = 0; i < N; i++) ref += price[i];
const got = df.sum("price");
console.log("  sum      rel.err " + (Math.abs(got - ref) / ref).toExponential(2) +
            "   (reordered additions: tolerance, not equality)");
let refm = 0, refc = 0;
for (let i = 0; i < N; i++) if (price[i] > 250) { refm += price[i]; refc++; }
console.log("  masked   rel.err " + (Math.abs(df.sum("price", mask) - refm) / refm).toExponential(2) +
            "   count " + (df.count("price", mask) === refc ? "EXACT" : "MISMATCH"));
let refq = 0; for (let i = 0; i < N; i++) refq += qty[i];
console.log("  int sum  " + (df.sum("qty") === refq ? "EXACT" : "MISMATCH " + df.sum("qty") + " vs " + refq) +
            "   (int64 accumulator, so this one is equality)");
const gref = new Float64Array(NGROUPS);
for (let i = 0; i < N; i++) gref[keyc[i]] += price[i];
const gg = df.groupBySum("keyc", "price");
let gmax = 0;
for (let i = 0; i < NGROUPS; i++) gmax = Math.max(gmax, Math.abs(gg.values[i] - gref[i]) / Math.max(1, gref[i]));
console.log("  groupby  max rel.err " + gmax.toExponential(2));

}   /* RUN_LEGACY */
