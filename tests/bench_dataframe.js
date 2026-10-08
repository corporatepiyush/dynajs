import { DataFrame } from "dyna:dataframe";

const ARGS = (typeof scriptArgs === "object" && scriptArgs) ? scriptArgs : [];
const WANT = ARGS.filter((a) => a === "agg" || a === "legacy");
const RUN_AGG = WANT.length === 0 || WANT.indexOf("agg") >= 0;
const RUN_LEGACY = WANT.length === 0 || WANT.indexOf("legacy") >= 0;

if (RUN_AGG) {
    let SINK = 0;

    const KS = [1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5, 8.5];
    let kc = 0;
    const K = () => KS[(kc++) & 7];

    function nsPerCall(f, reps) {
        for (let r = 0; r < (reps < 50 ? reps : 50); r++) SINK += f();
        let best = Infinity;
        for (let t = 0; t < 3; t++) {
            const t0 = performance.now();
            for (let r = 0; r < reps; r++) SINK += f();
            const d = (performance.now() - t0) * 1e6 / reps;
            if (d < best) best = d;
        }
        return best;
    }

    let ctr = 0;
    const noop = () => ++ctr;
    const CALARR = new Float64Array(8);
    const noopArr = () => CALARR.length;

    const SIZES = [100, 10000, 1000000];
    const rows = [];

    function build(n) {
        const f64 = new Float64Array(n);
        const f64b = new Float64Array(n);
        const f64pos = new Float64Array(n);
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
        const mask = df.GT("f64", 250);
        return {
            n, df, mask, f64,
            allOnes: new Uint8Array(n).fill(1),
            allZero: new Uint8Array(n),
            firstTrue: (() => { const u = new Uint8Array(n); if (n) u[0] = 1; return u; })(),
            firstFalse: (() => { const u = new Uint8Array(n).fill(1); if (n) u[0] = 0; return u; })(),
        };
    }

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
        const reps = Math.max(30, Math.ceil(3e7 / n));
        const slow = Math.max(5, reps >> 4);
        const A = true;

        row("sum.f64", n, () => df.SUM("f64"), reps);
        row("sum.f64.masked", n, () => df.SUM("f64", mask), reps);
        row("sum.i32", n, () => df.SUM("i32"), reps);
        row("sum.u8", n, () => df.SUM("u8"), reps);
        row("mean.f64", n, () => df.MEAN("f64"), reps);
        row("min.f64", n, () => df.MIN("f64"), reps);
        row("max.f64", n, () => df.MAX("f64"), reps);
        row("min.i32", n, () => df.MIN("i32"), reps);
        row("max.i32", n, () => df.MAX("i32"), reps);
        row("min.f64.masked", n, () => df.MIN("f64", mask), reps);
        row("count.masked", n, () => df.COUNT("f64", mask), reps);
        row("product.f64", n, () => df.PRODUCT("f64"), reps);
        row("product.i32", n, () => df.PRODUCT("i32"), reps);
        row("product.f64.masked", n, () => df.PRODUCT("f64", mask), reps);
        row("dot.f64", n, () => df.DOT_PRODUCT("f64", "f64b"), reps);
        row("dot.f64.masked", n, () => df.DOT_PRODUCT("f64", "f64b", mask), reps);
        row("dot.i32", n, () => df.DOT_PRODUCT("i32", "i32"), reps);
        row("dot.u8xi32.generic", n, () => df.DOT_PRODUCT("u8", "i32"), reps);
        row("variance.f64", n, () => df.VARIANCE("f64"), reps);
        row("variance.f64.masked", n, () => df.VARIANCE("f64", mask), reps);
        row("stddev.f64", n, () => df.STDDEV("f64"), reps);
        row("variance.i32", n, () => df.VARIANCE("i32"), reps);
        row("bitwiseAnd.i32", n, () => df.BITWISE_AND("i32"), reps);
        row("bitwiseOr.i32", n, () => df.BITWISE_OR("i32"), reps);
        row("bitwiseXor.i32", n, () => df.BITWISE_XOR("i32"), reps);
        row("bitwiseXor.u8", n, () => df.BITWISE_XOR("u8"), reps);
        row("bitwiseAnd.masked", n, () => df.BITWISE_AND("i32", mask), reps);

        row("ADV.all.allTrue", n, () => (df.ALL(ones) ? 1 : 0), reps);
        row("ADV.any.allFalse", n, () => (df.ANY(zero) ? 1 : 0), reps);
        row("easy.any.firstTrue", n, () => (df.ANY(c.firstTrue) ? 1 : 0), reps);
        row("easy.all.firstFalse", n, () => (df.ALL(c.firstFalse) ? 1 : 0), reps);
        row("any.mixed", n, () => (df.ANY(mask) ? 1 : 0), reps);

        row("bitmask", n, () => df.BITMASK(mask).length, reps, A);
        row("abs.f64", n, () => df.ABS("f64").length, reps, A);
        row("abs.i32", n, () => df.ABS("i32").length, reps, A);
        row("round.f64", n, () => df.ROUND("f64").length, reps, A);
        row("floor.f64", n, () => df.FLOOR("f64").length, reps, A);
        row("ceil.f64", n, () => df.CEIL("f64").length, reps, A);
        row("sign.f64", n, () => df.SIGN("f64").length, reps, A);
        row("sqrt.f64", n, () => df.SQRT("f64pos").length, slow, A);
        row("log.f64", n, () => df.LOG("f64pos").length, slow, A);
        row("exp.f64", n, () => df.EXP("f64b").length, slow, A);
        row("isna.f64", n, () => df.IS_NA("f64").length, reps, A);
        row("notna.f64", n, () => df.NOT_NA("f64").length, reps, A);
        row("isna.i32", n, () => df.IS_NA("i32").length, reps, A);
        row("between.f64", n, () => df.BETWEEN("f64", K(), 400).length, reps, A);
        row("clip.f64", n, () => df.CLIP("f64", K(), 400).length, reps, A);
        row("fillna.f64", n, () => df.FILL_NA("f64", K()).length, reps, A);
        row("add.col", n, () => df.ADD("f64", "f64b").length, reps, A);
        row("add.scalar", n, () => df.ADD("f64", K()).length, reps, A);
        row("sub.col", n, () => df.SUB("f64", "f64b").length, reps, A);
        row("mul.col", n, () => df.MUL("f64", "f64b").length, reps, A);
        row("div.col", n, () => df.DIV("f64", "f64b").length, reps, A);
        row("rsub.scalar", n, () => df.RSUB("f64", K()).length, reps, A);
        row("rdiv.scalar", n, () => df.RDIV("f64", K()).length, reps, A);
        row("add.widen.i32", n, () => df.ADD("i32", "f64b").length, reps, A);
        row("pow.k2", n, () => df.POW("f64", 2).length, slow, A);
        row("pow.frac", n, () => df.POW("f64", 2.7).length, slow, A);
        row("pow.col", n, () => df.POW("f64b", "f64b").length, slow, A);
        row("where.cc", n, () => df.WHERE(mask, "f64", "f64b").length, reps, A);
        row("where.cs", n, () => df.WHERE(mask, "f64", K()).length, reps, A);
        row("where.ss", n, () => df.WHERE(mask, K(), 0).length, reps, A);
        row("eq.f64", n, () => df.EQ("f64", K()).length, reps, A);
        row("ne.f64", n, () => df.NE("f64", K()).length, reps, A);
        row("le.f64", n, () => df.LE("f64", K()).length, reps, A);
        row("ge.f64", n, () => df.GE("f64", K()).length, reps, A);
        row("lt.i32", n, () => df.LT("i32", 0).length, reps, A);

        row("CONTROL.gt", n, () => df.GT("f64", 250).length, reps, A);
        row("CONTROL.GROUP_BY_SUM", n,
            () => df.GROUP_BY_SUM("key", "f64").values[0], Math.max(20, reps >> 3));
        row("CONTROL.abs.map", n, () => df.ABS("f64b").length, reps, A);
        row("CONTROL.where.sel", n, () => df.WHERE(ones, "f64", "f64b").length, reps, A);
    }
    console.log("# sink " + (SINK === 12345 ? "?" : "ok"));

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
        const pv = new Float64Array(n);
        for (let i = 0; i < n; i++) pv[i] = (i & 1) ? 0.5 : 2;
        const df = new DataFrame({ x, y, iv, u, pv });
        const m = df.GT("x", 200);
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
            say("dotProduct", rel(df.DOT_PRODUCT("x", "y"), d));
            let dm = 0; for (let i = 0; i < n; i++) if (m[i]) dm += x[i] * y[i];
            say("dotProduct mask", rel(df.DOT_PRODUCT("x", "y", m), dm));
            let dg = 0; for (let i = 0; i < n; i++) dg += u[i] * iv[i];
            say("dot generic", rel(df.DOT_PRODUCT("u", "iv"), dg));
        }
        {
            let s = 0; for (let i = 0; i < n; i++) s += x[i];
            const mu = s / n;
            let v = 0; for (let i = 0; i < n; i++) v += (x[i] - mu) * (x[i] - mu);
            say("variance", rel(df.VARIANCE("x"), v / (n - 1)));
            say("stddev", rel(df.STDDEV("x"), Math.sqrt(v / (n - 1))));
        }
        {
            let p = 1; for (let i = 0; i < n; i++) p *= pv[i];
            say("product", rel(df.PRODUCT("pv"), p));
            if (!Number.isFinite(p))
                console.log("  ** the product reference overflowed; this row is not a test **");
            let a = -1, o = 0, xr = 0;
            for (let i = 0; i < n; i++) { a &= iv[i]; o |= iv[i]; xr ^= iv[i]; }
            say("bitwiseAnd", rel(df.BITWISE_AND("iv"), a));
            say("bitwiseOr", rel(df.BITWISE_OR("iv"), o));
            say("bitwiseXor", rel(df.BITWISE_XOR("iv"), xr));
            let su = 0; for (let i = 0; i < n; i++) su += u[i];
            say("sum.u8", rel(df.SUM("u"), su));
        }
        {
            const bm = df.BITMASK(m);
            let b2 = 0;
            for (let i = 0; i < n; i++)
                if (((bm[i >> 5] >>> (i & 31)) & 1) !== (m[i] ? 1 : 0)) b2++;
            if (b2) bad++;
            console.log("  " + "bitmask".padEnd(18) + (b2 ? "MISMATCH " + b2 : "ok"));
        }
        {
            const chk = (name, got, f) => {
                let w = -1;
                for (let i = 0; i < n; i++) {
                    const want = f(x[i]);
                    if (!Object.is(got[i], want) && !(Number.isNaN(got[i]) && Number.isNaN(want))) { w = i; break; }
                }
                if (w >= 0) bad++;
                console.log("  " + name.padEnd(18) + (w < 0 ? "ok (bit-identical)" : "MISMATCH at " + w));
            };
            chk("abs", df.ABS("x"), Math.abs);
            chk("round", df.ROUND("x"), Math.round);
            chk("sqrt", df.SQRT("x"), Math.sqrt);
            chk("log", df.LOG("x"), Math.log);
        }
        console.log(bad ? "  ** " + bad + " DIFFERENTIAL MISMATCH(ES): the numbers above are not "
                          + "measuring a correct kernel **"
                        : "  all differentials clean");
    }
}

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
for (let i = 0; i < N; i++) city.push(CITIES[i % NGROUPS]);

console.log("rows = " + N + ", groups = " + NGROUPS);

console.log("\n[1] unmasked sum -- vs the memory-bound floor");
const floor = bench("Float64Array.sum()  (floor)", () => price.sum());
const jsSum = bench("JS for-loop sum", () => { let s = 0; for (let i = 0; i < N; i++) s += price[i]; return s; });
const t0 = performance.now();
const df = new DataFrame({ price, qty, keyc, city });
const ctorMs = performance.now() - t0;
const dfSum = bench("df.SUM('price')", () => df.SUM("price"));
console.log("  -> vs JS loop " + (jsSum / dfSum).toFixed(1) + "x, " +
            "vs floor " + (dfSum / floor).toFixed(2) + "x");
console.log("  (DataFrame construction, incl. dictionary-encoding " + N +
            " strings: " + ctorMs.toFixed(1) + " ms, one time)");

console.log("\n[2] predicate -> mask");
const jsFilter = bench("JS filter count (price>250)", () => { let c = 0; for (let i = 0; i < N; i++) if (price[i] > 250) c++; return c; });
const dfMask = bench("df.GT('price',250)", () => df.GT("price", 250));
console.log("  -> " + (jsFilter / dfMask).toFixed(1) + "x");

console.log("\n[3] MASKED reduction -- no engine primitive exists for this");
const mask = df.GT("price", 250);
const jsMasked = bench("JS masked sum", () => { let s = 0; for (let i = 0; i < N; i++) if (price[i] > 250) s += price[i]; return s; });
const dfMasked = bench("df.SUM('price', mask)", () => df.SUM("price", mask));
console.log("  -> " + (jsMasked / dfMasked).toFixed(1) + "x");

console.log("\n[4] GROUP-BY sum -- no engine primitive exists for this");
const jsGroup = bench("JS groupby-sum (int keys)", () => {
    const g = new Float64Array(NGROUPS);
    for (let i = 0; i < N; i++) g[keyc[i]] += price[i];
    return g[0];
});
const dfGroupI = bench("df.GROUP_BY_SUM('keyc','price')", () => df.GROUP_BY_SUM("keyc", "price"));
const dfGroupS = bench("df.GROUP_BY_SUM('city','price')", () => df.GROUP_BY_SUM("city", "price"));
console.log("  -> int keys " + (jsGroup / dfGroupI).toFixed(1) + "x, " +
            "string keys " + (jsGroup / dfGroupS).toFixed(1) +
            "x (a JS string-keyed groupby with a Map is far slower still)");

console.log("\n[5] the same pipeline end to end (filter -> group -> sum)");
const jsPipe = bench("JS: filter+groupby", () => {
    const g = new Float64Array(NGROUPS);
    for (let i = 0; i < N; i++) if (price[i] > 250) g[keyc[i]] += price[i];
    return g[0];
});
const dfPipe = bench("df: gt + GROUP_BY_SUM(mask)", () => {
    const m = df.GT("price", 250);
    return df.GROUP_BY_SUM("keyc", "price", m);
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
    const a = new DataFrame({ v: df.MUL("price", 1.2) });
    const b = new DataFrame({ v: a.ADD("v", 3) });
    return b.CLIP("v", 10, 400)[0];
});
console.log("  -> " + (jsMap / dfMap).toFixed(1) +
            "x (three passes and two intermediate frames against one fused JS loop:" +
            " the losing shape, reported because it is the one users write)");

console.log("\n[7] results agree with the JS loops");
let ref = 0; for (let i = 0; i < N; i++) ref += price[i];
const got = df.SUM("price");
console.log("  sum      rel.err " + (Math.abs(got - ref) / ref).toExponential(2) +
            "   (reordered additions: tolerance, not equality)");
let refm = 0, refc = 0;
for (let i = 0; i < N; i++) if (price[i] > 250) { refm += price[i]; refc++; }
console.log("  masked   rel.err " + (Math.abs(df.SUM("price", mask) - refm) / refm).toExponential(2) +
            "   count " + (df.COUNT("price", mask) === refc ? "EXACT" : "MISMATCH"));
let refq = 0; for (let i = 0; i < N; i++) refq += qty[i];
console.log("  int sum  " + (df.SUM("qty") === refq ? "EXACT" : "MISMATCH " + df.SUM("qty") + " vs " + refq) +
            "   (int64 accumulator, so this one is equality)");
const gref = new Float64Array(NGROUPS);
for (let i = 0; i < N; i++) gref[keyc[i]] += price[i];
const gg = df.GROUP_BY_SUM("keyc", "price");
let gmax = 0;
for (let i = 0; i < NGROUPS; i++) gmax = Math.max(gmax, Math.abs(gg.values[i] - gref[i]) / Math.max(1, gref[i]));
console.log("  groupby  max rel.err " + gmax.toExponential(2));

}
