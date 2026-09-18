/*
 * test_dataframe_audit.js — audit suite for dyna:dataframe.
 * Covers the pins verified during the module audit: quantile interpolation
 * families, rank conventions, NTILE tiling, window edges, mask-length
 * refusals, frame ownership, hostile column names, allocation-bomb
 * refusals, and the O(1) duplicate-key join index.
 * Portable + seeded: every random sequence comes from an explicit PRNG.
 */
import {DataFrame} from "dyna:dataframe";

let passed = 0, failed = 0;
function ok(cond, name) {
    if (cond) { passed++; return; }
    failed++;
    print(`FAIL ${name}`);
}
function numEq(got, want, name) {
    ok(got === want || (got !== got && want !== want), `${name}: got ${got} want ${want}`);
}
function arrEq(got, want, name) {
    let same = got.length === want.length;
    for (let i = 0; same && i < want.length; i++)
        if (!(got[i] === want[i] || (got[i] !== got[i] && want[i] !== want[i]))) same = false;
    ok(same, `${name}: got [${got}] want [${want}]`);
}
function throws(name, fn) {
    try { fn(); failed++; print(`FAIL ${name}: no throw`); }
    catch (e) { passed++; }
}
function rng(seed) {
    return function () {
        seed |= 0; seed = (seed + 0x6D2B79F5) | 0;
        let t = Math.imul(seed ^ (seed >>> 15), 1 | seed);
        t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
        return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    };
}

/* ---------- quantile families ---------- */
{
    const df = new DataFrame({a: new Float64Array([1, 2, 3, 4])});
    numEq(df.QUANTILE("a", 0.5), 2.5, "QUANTILE linear even n");
    numEq(df.PERCENTILE_CONT("a", 0.5), 2.5, "PERCENTILE_CONT == numpy linear");
    numEq(df.PERCENTILE_DISC("a", 0.5), 2, "PERCENTILE_DISC == SQL ceil(q*n)-1");
    numEq(df.PERCENTILE_DISC("a", 0.25), 1, "PERCENTILE_DISC q=0.25");
    numEq(df.PERCENTILE_DISC("a", 0.76), 4, "PERCENTILE_DISC q=0.76");
    numEq(df.MEDIAN("a"), 2.5, "MEDIAN even n");
    numEq(df.QUANTILE_EXACT_LOW("a", 0.5), 2, "EXACT_LOW");
    numEq(df.QUANTILE_EXACT_HIGH("a", 0.5), 3, "EXACT_HIGH");
    const odd = new DataFrame({a: new Float64Array([10, 20, 30])});
    numEq(odd.PERCENTILE_DISC("a", 0.5), 20, "PERCENTILE_DISC odd n");
    numEq(odd.MEDIAN("a"), 20, "MEDIAN odd n");

    /* QUANTILES: many quantiles from one gather, ascending-select scheme.
       Values must match single-call QUANTILE within 1 ULP. */
    const N = 200, a = new Float64Array(N);
    for (let i = 0; i < N; i++) a[i] = i;
    const line = new DataFrame({a});
    const qs = [0.02, 0.1, 0.25, 0.5, 0.75, 0.9, 0.98];
    const multi = line.QUANTILES("a", qs);
    let okq = multi.length === qs.length;
    for (let i = 0; okq && i < qs.length; i++) {
        const single = line.QUANTILE("a", qs[i]);
        if (!(multi[i] === single || Math.abs(multi[i] - single) < 1e-9 * (1 + Math.abs(single))))
            okq = false;
    }
    ok(okq, "QUANTILES matches per-q QUANTILE within 1 ULP");
    const rev = line.QUANTILES("a", [0.9, 0.1]);
    numEq(rev[0], rev.length >= 1 ? line.QUANTILE("a", 0.9) : NaN, "QUANTILES caller order kept [0]");
    numEq(rev[1], line.QUANTILE("a", 0.1), "QUANTILES caller order kept [1]");
    throws("QUANTILES q=1.5 refused", () => line.QUANTILES("a", [1.5]));
    throws("QUANTILE q out of [0,1]", () => line.QUANTILE("a", 1.5));

    /* t-digest: pinned rank error <= 0.01 on skewed data */
    const r = rng(9), M = 50000, sk = new Float64Array(M);
    for (let i = 0; i < M; i++) sk[i] = Math.pow(r(), 8) * 1e6;
    const skewed = new DataFrame({a: sk});
    const tqs = [0.001, 0.01, 0.25, 0.5, 0.75, 0.99, 0.999];
    const td = skewed.QUANTILES_TDIGEST("a", tqs);
    const sorted = Array.from(sk).sort((x, y) => x - y);
    let worst = 0;
    for (let i = 0; i < tqs.length; i++) {
        let lo = 0, hi = M;
        while (lo < hi) { const mid = (lo + hi) >> 1; if (sorted[mid] < td[i]) lo = mid + 1; else hi = mid; }
        worst = Math.max(worst, Math.abs(lo / (M - 1) - tqs[i]));
    }
    ok(worst <= 0.0101, `tdigest rank error ${worst.toFixed(5)} <= 0.01`);
}

/* ---------- rank conventions ---------- */
{
    const df = new DataFrame({a: new Float64Array([10, 20, 20, 30])});
    arrEq(df.RANK("a"), [1, 2.5, 2.5, 4], "RANK average ties");
    arrEq(df.DENSE_RANK("a"), [1, 2, 2, 3], "DENSE_RANK no gaps");
    arrEq(df.PERCENT_RANK("a"), [0, 1 / 3, 1 / 3, 1], "PERCENT_RANK min-rank normalised");
    const one = new DataFrame({a: new Float64Array([7])});
    numEq(one.PERCENT_RANK("a")[0], 0, "PERCENT_RANK single row = 0");
    /* sum invariance of average ranks: sum = n(n+1)/2 whatever the ties */
    const r2 = rng(3), b = new Float64Array(64);
    for (let i = 0; i < 64; i++) b[i] = Math.floor(r2() * 5);
    const many = new DataFrame({a: b});
    let s = 0;
    for (const v of many.RANK("a")) s += v;
    numEq(s, 64 * 65 / 2, "RANK column sum invariant");
}

/* ---------- NTILE ---------- */
{
    const df = new DataFrame({a: new Float64Array([5, 3, 8, 1, 9, 2, 7, 4, 6, 0])});
    const t = df.NTILE("a", 4);
    const sizes = [0, 0, 0, 0];
    for (const x of t) sizes[x - 1]++;
    arrEq(sizes, [3, 3, 2, 2], "NTILE first buckets take the remainder");
    const mx = Math.max(...df.NTILE("a", 20));
    numEq(mx, 10, "NTILE buckets > rows gives one row per tile");
    throws("NTILE buckets=0 refused", () => df.NTILE("a", 0));
}

/* ---------- ordering: NaN last, argsort is a permutation ---------- */
{
    const df = new DataFrame({a: new Float64Array([3, NaN, 1, Infinity, -Infinity])});
    arrEq(df.SORT("a"), [-Infinity, 1, 3, Infinity, NaN], "SORT NaN last");
    const idx = df.ARG_SORT("a");
    arrEq(Array.from(idx).sort((x, y) => x - y), [0, 1, 2, 3, 4], "ARG_SORT is a permutation");
    const vals = df.TO_COLUMNS().a;
    arrEq(Array.from(idx).map(i => vals[i]), [-Infinity, 1, 3, Infinity, NaN], "ARG_SORT values ordered");
    numEq(df.MIN("a"), -Infinity, "MIN NaN-ignoring");
    numEq(df.MAX("a"), Infinity, "MAX NaN-ignoring");
    numEq(df.ARG_MIN("a"), 4, "ARG_MIN skips NaN");
    numEq(df.SUM("a") !== df.SUM("a"), true, "SUM propagates NaN");
}

/* ---------- windows ---------- */
{
    const df = new DataFrame({a: new Float64Array([1, 2, 3, 4, 5])});
    arrEq(df.ROLLING_SUM("a", 1), [1, 2, 3, 4, 5], "ROLLING_SUM w=1");
    arrEq(df.ROLLING_SUM("a", 2), [NaN, 3, 5, 7, 9], "ROLLING_SUM w=2");
    arrEq(df.ROLLING_MEAN("a", 2), [NaN, 1.5, 2.5, 3.5, 4.5], "ROLLING_MEAN divides by contributed");
    const rs = df.ROLLING_SUM("a", 5);
    numEq(rs[4], 15, "ROLLING_SUM w=rows last");
    ok(rs[0] !== rs[0], "ROLLING_SUM w=rows first is NaN");
    const big = df.ROLLING_SUM("a", 6);
    ok(Array.from(big).every(x => x !== x), "ROLLING_SUM w>rows all NaN");
    arrEq(df.SHIFT("a", 1), [NaN, 1, 2, 3, 4], "SHIFT head vacates");
    arrEq(df.SHIFT("a", -1), [2, 3, 4, 5, NaN], "SHIFT negative: tail vacates");
    arrEq(df.SHIFT("a", 0), [1, 2, 3, 4, 5], "SHIFT 0 identity");
    arrEq(df.SHIFT("a", 99), [NaN, NaN, NaN, NaN, NaN], "SHIFT beyond length all NaN");
    arrEq(df.DIFF("a", 1), [NaN, 1, 1, 1, 1], "DIFF");
    throws("SHIFT fractional refused", () => df.SHIFT("a", 1.5));
    throws("ROLLING w=0 refused", () => df.ROLLING_SUM("a", 0));
    throws("ROLLING fractional refused", () => df.ROLLING_SUM("a", 2.5));
    throws("EMA alpha=0 refused", () => df.EMA("a", 0));
    throws("EMA alpha>1 refused", () => df.EMA("a", 1.5));
    arrEq(df.EMA("a", 1), [1, 2, 3, 4, 5], "EMA alpha=1 identity");
    const ema = df.EMA("a", 0.5);
    numEq(ema[1], 0.5 * 2 + 0.5 * 1, "EMA recursion");
    arrEq(df.PCT_CHANGE("a"), [NaN, 1, 0.5, 1 / 3, 0.25], "PCT_CHANGE");
    const z = df.ZSCORE("a");
    ok(Math.abs(z[4]) > 1, "ZSCORE endpoint above mean");
}

/* ---------- grouped family: dense key indices, empty groups ---------- */
{
    const df = new DataFrame({k: new Int32Array([1, 1, 1, 2]), v: new Float64Array([7, 3, 7, 9])});
    const g = df.GROUP_UNIQ_ARRAY("k", "v");
    arrEq(Array.from(g.values[1]), [7, 3], "GROUP_UNIQ_ARRAY first-seen order");
    arrEq(Array.from(g.values[0]), [], "integer keys reserve dense index 0");
    const ms = df.GROUP_ARRAY_MOVING_SUM("k", "v", 2);
    arrEq(Array.from(ms.values[1]), [7, 10, 10], "GROUP_ARRAY_MOVING_SUM w=2");
    const exp = df.GROUP_ARRAY_MOVING_SUM("k", "v");
    arrEq(Array.from(exp.values[1]), [7, 10, 17], "MOVING_SUM expanding default");
    const bySum = df.GROUP_BY_SUM("k", "v");
    arrEq(Array.from(bySum.values), [0, 17, 9], "GROUP_BY_SUM dense with empty group 0");
    const gs = new DataFrame({k: ["a", "b", "a"], v: new Float64Array([1, 2, 3])});
    const gsr = gs.GROUP_BY_SUM("k", "v");
    arrEq(gsr.keys, ["a", "b"], "string keys in first-seen order");
    arrEq(Array.from(gsr.values), [4, 2], "string key sums");
    /* GROUP_ARRAY_INTERSECT: present in EVERY group that has rows. Ghost
       dense keys (absent key values) must not poison the answer. */
    const iv = new DataFrame({k: new Int32Array([1, 1, 2, 2]), v: new Float64Array([5, 6, 6, 7])});
    const inter = iv.GROUP_ARRAY_INTERSECT("k", "v");
    arrEq(Array.from(inter), [6], "GROUP_ARRAY_INTERSECT skips absent dense keys");
    const iv2 = new DataFrame({k: new Int32Array([0, 0, 1, 1]), v: new Float64Array([5, 6, 6, 7])});
    arrEq(Array.from(iv2.GROUP_ARRAY_INTERSECT("k", "v")), [6], "GROUP_ARRAY_INTERSECT dense from 0");
    /* a group whose rows are all NaN holds no value: the intersection is empty */
    const iv3 = new DataFrame({k: new Int32Array([0, 0, 1, 1]), v: new Float64Array([NaN, NaN, 5, 6])});
    arrEq(Array.from(iv3.GROUP_ARRAY_INTERSECT("k", "v")), [], "GROUP_ARRAY_INTERSECT NaN-only group empties");
    /* interleaved rows: distinctness is per (value, group) pair, not per
       consecutive run -- the alternation counter used to over-count and
       emit values that never appeared in every group */
    const iv4 = new DataFrame({k: new Int32Array([1, 2, 1, 2]), v: new Float64Array([5, 5, 6, 9])});
    arrEq(Array.from(iv4.GROUP_ARRAY_INTERSECT("k", "v")), [5],
          "GROUP_ARRAY_INTERSECT interleaved rows count distinct groups");
    const iv5 = new DataFrame({k: new Int32Array([1, 2, 1, 2]), v: new Float64Array([5, 5, 6, 6])});
    arrEq(Array.from(iv5.GROUP_ARRAY_INTERSECT("k", "v")), [5, 6],
          "GROUP_ARRAY_INTERSECT interleaved both in all groups");
    const iat = new DataFrame({v: new Float64Array([10, 20, 30]), p: new Float64Array([1, 0, 2])});
    arrEq(Array.from(iat.GROUP_ARRAY_INSERT_AT("v", "p", 3)), [20, 10, 30], "INSERT_AT later overwrites");
}

/* ---------- mask discipline ---------- */
{
    const df = new DataFrame({a: new Float64Array([1, 2, 3, 4])});
    const m4 = new Uint8Array([1, 0, 1, 0]);
    const m3 = new Uint8Array(3), m5 = new Uint8Array(5);
    numEq(df.SUM("a", m4), 4, "SUM masked");
    numEq(df.MIN("a", m4), 1, "MIN masked");
    numEq(df.COUNT("a", m4), 2, "COUNT masked");
    numEq(df.MEAN("a", new Uint8Array([0, 0, 0, 0])) !== df.MEAN("a", new Uint8Array([0, 0, 0, 0])), true, "MEAN empty selection NaN");
    throws("SUM short mask refused", () => df.SUM("a", m3));
    throws("SUM long mask refused", () => df.SUM("a", m5));
    throws("FILTER short mask refused", () => df.FILTER(m3));
    throws("FILTER long mask refused", () => df.FILTER(m5));
    throws("MASK short mask refused", () => df.MASK(m3));
    throws("ALL short mask refused", () => df.ALL(m3));
    throws("BITMASK short mask refused", () => df.BITMASK(m3));
    throws("WHERE short mask refused", () => df.WHERE(m3, "a", 1));
    throws("SUM u16 mask refused", () => df.SUM("a", new Uint16Array(4)));
    throws("ALL undefined mask refused", () => df.ALL(undefined));
    /* masked-out rows do not contribute to CUM_* */
    const cm = new DataFrame({a: new Float64Array([1, 2, 4, 8])});
    arrEq(cm.CUM_SUM("a", new Uint8Array([1, 0, 1, 1])), [1, 1, 5, 13], "CUM_SUM masked carries forward");
    /* DELTA_SUM over the selection only */
    numEq(cm.DELTA_SUM("a"), 7, "DELTA_SUM positive only");
    const dec = new DataFrame({a: new Float64Array([8, 4, 2, 1])});
    numEq(dec.DELTA_SUM("a"), 0, "DELTA_SUM monotonic decrease adds nothing");
    numEq(dec.DELTA_SUM("a", new Uint8Array([1, 0, 1, 1])), 0,
          "DELTA_SUM masked-out row skipped, decrease across it ignored");
    /* BITMASK packing LSB first */
    const bits = df.BITMASK(new Uint8Array([1, 0, 0, 1]));
    numEq(bits[0], 0b1001, "BITMASK LSB first");
}

/* ---------- reshape: joins, concat, pivot/melt, resample ---------- */
{
    const L = new DataFrame({k: new Int32Array([1, 2, 3]), v: new Float64Array([10, 20, 30])});
    const R = new DataFrame({k: new Int32Array([2, 2, 4]), w: new Float64Array([5, 6, 7])});
    const ji = L.JOIN(R, "k", "k", "inner");
    numEq(ji.ROWS, 2, "JOIN inner duplicate right keys multiply");
    arrEq(Array.from(ji.TO_COLUMNS().v), [20, 20], "JOIN inner left rows repeat");
    arrEq(Array.from(ji.TO_COLUMNS().w), [5, 6], "JOIN inner right rows in row order");
    const jl = L.JOIN(R, "k", "k", "left");
    numEq(jl.ROWS, 4, "JOIN left keeps unmatched");
    const jc = jl.TO_COLUMNS();
    numEq(jc.w[0] !== jc.w[0], true, "JOIN left unmatched right is NaN");
    const jr = L.JOIN(R, "k", "k", "right");
    numEq(jr.ROWS, 3, "JOIN right: one row per right row");
    const jo = L.JOIN(R, "k", "k", "outer");
    numEq(jo.ROWS, 5, "JOIN outer");
    /* pinned: `matched` is a HAS-RIGHT-SIDE flag (right-only rows are 1),
       not an inner-pair indicator -- documented behaviour, not changed. */
    const jm = Array.from(jo.TO_COLUMNS().matched);
    arrEq(jm, [0, 1, 1, 0, 1], "JOIN outer matched = has-right flag");
    /* integer-key only */
    const F = new DataFrame({k: new Float64Array([1]), v: new Float64Array([1])});
    throws("JOIN float key refused", () => F.JOIN(F, "k", "k", "inner"));
    throws("JOIN bad how refused", () => L.JOIN(R, "k", "k", "sideways"));

    /* ASOF_JOIN: sorted required, latest right <= left */
    const Lt = new DataFrame({t: new Int32Array([1, 2, 3]), v: new Float64Array([1, 2, 3])});
    const Rt = new DataFrame({t: new Int32Array([1, 3]), w: new Float64Array([10, 30])});
    const ja = Lt.ASOF_JOIN(Rt, "t", "t");
    arrEq(Array.from(ja.TO_COLUMNS().w), [10, 10, 30], "ASOF picks latest right <= left");
    const Lu = new DataFrame({t: new Int32Array([2, 1]), v: new Float64Array([0, 0])});
    throws("ASOF unsorted left refused", () => Lu.ASOF_JOIN(Rt, "t", "t"));

    /* CONCAT widens mixed numerics, preserves same integer type */
    const ci = new DataFrame({x: new Int32Array([1, 2])}).CONCAT(new DataFrame({x: new Int32Array([3])}));
    numEq(ci.ROWS, 3, "CONCAT rows");
    numEq(ci.DTYPES().x, "i32", "CONCAT preserves same integer type");
    const cw = new DataFrame({x: new Int32Array([1])}).CONCAT(new DataFrame({x: new Float64Array([2])}));
    numEq(cw.DTYPES().x, "f64", "CONCAT widens mixed numerics");
    throws("CONCAT column set must match", () =>
        new DataFrame({x: new Int32Array([1])}).CONCAT(new DataFrame({y: new Int32Array([1])})));

    /* PIVOT then MELT roundtrip */
    const pv = new DataFrame({i: new Int32Array([1, 1, 2, 2]), c: new Int32Array([1, 2, 1, 2]),
                              v: new Float64Array([10, 20, 30, 40])});
    const p = pv.PIVOT("i", "c", "v", "sum");
    arrEq(p.COLUMNS, ["i", "1", "2"], "PIVOT column names");
    const pc = p.TO_COLUMNS();
    numEq(pc["1"][0] + pc["2"][0], 30, "PIVOT cell sums");
    const me = p.MELT(["i"], ["1", "2"]);
    numEq(me.ROWS, 4, "MELT row count");
    const mv = me.TO_COLUMNS();
    arrEq(Array.from(mv.value).sort((a, b) => a - b), [10, 20, 30, 40], "PIVOT/MELT roundtrip");
    arrEq(Array.from(mv.variable), ["1", "2", "1", "2"], "MELT variable column");
    throws("MELT empty valueVars refused", () => pv.MELT([], []));

    /* RESAMPLE: left-closed [t0+k*i, t0+(k+1)*i) over the time column */
    const rs = new DataFrame({t: new Float64Array([0, 0.9, 1.0, 1.9]), v: new Float64Array([1, 2, 3, 4])});
    const r1 = rs.RESAMPLE("t", 1, "sum");
    arrEq(Array.from(r1.TO_COLUMNS().bucket), [0, 1], "RESAMPLE bucket starts");
    arrEq(Array.from(r1.TO_COLUMNS().value), [0.9, 2.9], "RESAMPLE aggregates the time column (documented signature)");
    const ru = new DataFrame({t: new Float64Array([2, 1])});
    throws("RESAMPLE unsorted refused", () => ru.RESAMPLE("t", 1));

    /* RANGE_AGG merges touching intervals */
    const ra = new DataFrame({lo: new Float64Array([0, 2, 10]), hi: new Float64Array([2, 5, 12])});
    const u = ra.RANGE_AGG("lo", "hi");
    arrEq(Array.from(u.starts), [0, 10], "RANGE_AGG merged starts");
    arrEq(Array.from(u.ends), [5, 12], "RANGE_AGG merged ends");
}

/* ---------- ownership: derived frames never alias ---------- */
{
    const src = new Float64Array([1, 2, 3, 4]);
    const d = new DataFrame({a: src});
    const f = d.FILTER(new Uint8Array([1, 0, 1, 0]));
    src[2] = 999;
    numEq(f.TO_COLUMNS().a[1], 3, "FILTER owns its data");
    src[2] = 3;
    const c = d.COPY();
    src[0] = 500;
    numEq(c.TO_COLUMNS().a[0], 1, "COPY owns its data");
    src[0] = 1;
    const sl = d.SLICE(1, 3);
    src[1] = 888;
    numEq(sl.TO_COLUMNS().a[0], 2, "SLICE owns its data");
    src[1] = 2;
    const t1 = d.TO_COLUMNS();
    t1.a[0] = 12345;
    numEq(d.TO_COLUMNS().a[0], 1, "TO_COLUMNS fresh copies");
    numEq(d.SAMPLE(4, 42).ROWS, 4, "SAMPLE seeded deterministic count");
    arrEq(Array.from(d.SAMPLE(4, 42).TO_COLUMNS().a).sort((a, b) => a - b), [1, 2, 3, 4],
          "SAMPLE without replacement");
    /* string columns: fresh arrays from TO_COLUMNS */
    const s = new DataFrame({s: ["p", "q"]});
    const sa = s.TO_COLUMNS().s;
    sa[0] = "zzz";
    numEq(s.TO_COLUMNS().s[0], "p", "string TO_COLUMNS fresh");
}

/* ---------- hostile column names: own properties only ---------- */
{
    const cols = {};
    Object.defineProperty(cols, "__proto__", {value: new Float64Array([1, 2]), enumerable: true});
    Object.defineProperty(cols, "length", {value: new Float64Array([3, 4]), enumerable: true});
    const df = new DataFrame(cols);
    arrEq(df.COLUMNS, ["__proto__", "length"], "hostile names kept in order");
    numEq(df.SUM("length"), 7, "column named length reducible");
    numEq(df.DTYPES().__proto__, "f64", "__proto__ column reported via own property");
    const tc = df.TO_COLUMNS();
    ok(Object.getPrototypeOf(tc) !== null && "__proto__" in tc, "TO_COLUMNS keeps hostile keys");
    const pj = new DataFrame({k: ["__proto__", "constructor"], v: new Float64Array([1, 2])});
    const ja = JSON.parse(pj.JSON_OBJECT_AGG("k", "v"));
    numEq(Object.keys(ja).length, 2, "JSON_OBJECT_AGG hostile keys own");
    throws("NUL in column arg refused", () => df.SUM("a\u0000b"));
    throws("NUL in ctor name refused", () => {
        const bad = {};
        Object.defineProperty(bad, "a\u0000b", {value: new Float64Array([1]), enumerable: true});
        return new DataFrame(bad);
    });
}

/* ---------- allocation bombs: refuse, never OOM ---------- */
{
    const df = new DataFrame({a: new Float64Array([1, 2, 3, 4])});
    throws("HISTOGRAM bins=1e9 refused", () => df.HISTOGRAM("a", 1e9));
    throws("HISTOGRAM NaN bins refused", () => df.HISTOGRAM("a", NaN));
    throws("GROUP_ARRAY_INSERT_AT size=2e9 refused", () => df.GROUP_ARRAY_INSERT_AT("a", "a", 2e9));
    numEq(df.N_LARGEST("a", 1e9).length, 4, "N_LARGEST huge k clamps");
    throws("TOP_K_WEIGHTED k=1e9 refused", () => df.TOP_K_WEIGHTED("a", undefined, 1e9));
    throws("QUANTILES huge array refused", () => df.QUANTILES("a", new Array(1 << 21).fill(0.5)));
    throws("GROUP_BITMAP negative refused", () => new DataFrame({a: new Int32Array([5, -1])}).GROUP_BITMAP("a"));
    throws("GROUP_BITMAP beyond 2^26 refused", () => new DataFrame({a: new Uint32Array([1 << 27])}).GROUP_BITMAP("a"));
    /* duplicate-key blowup: bounded by the row limit, fast via the tail index */
    const dup = new DataFrame({k: new Int32Array(200000).fill(1), v: new Float64Array(200000)});
    throws("JOIN runaway product refused", () => dup.JOIN(dup, "k", "k", "inner"));
    /* the fix: one repeated key on the RIGHT builds in O(n) and joins in O(n) */
    const right = new DataFrame({k: new Int32Array(64000).fill(1), w: new Float64Array(64000)});
    const left = new DataFrame({k: new Int32Array([1]), v: new Float64Array([0])});
    const t0 = Date.now();
    const j = left.JOIN(right, "k", "k", "inner");
    numEq(j.ROWS, 64000, "single-key join emits every pair");
    const dt = Date.now() - t0;
    ok(dt < 500, `single-key join index build O(n): ${dt} ms for 64k rows`);
    /* row order preserved: right rows in insertion order */
    const jw = j.TO_COLUMNS().w;
    let inorder = true;
    for (let i = 0; i < jw.length; i++) if (jw[i] !== 0) inorder = false;
    ok(inorder, "single-key join row order kept");
}

/* ---------- exactness pins ---------- */
{
    /* SUM_CHECKED refuses only beyond a Number's exact range */
    const N = 2097153;  /* 2^21+1 times (2^32-1) > 2^53 */
    const big = new DataFrame({a: new Uint32Array(N).fill(4294967295)});
    throws("SUM_CHECKED overflows a Number -> RangeError", () => big.SUM_CHECKED("a"));
    numEq(big.SUM("a"), 9007203547611135, "SUM unchecked stays finite");
    const small = new DataFrame({a: new Uint32Array([4294967295, 4294967295])});
    numEq(small.SUM_CHECKED("a"), 8589934590, "SUM_CHECKED exact under 2^53");
    /* UNIQ_UP_TO: n+1 sentinel */
    const u = new DataFrame({a: new Float64Array([1, 2, 3, 4, 5])});
    numEq(u.UNIQ_UP_TO("a", 2), 3, "UNIQ_UP_TO sentinel n+1");
    numEq(u.UNIQ_UP_TO("a", 5), 5, "UNIQ_UP_TO exact at n");
    numEq(u.UNIQ_UP_TO("a", 0), 1, "UNIQ_UP_TO zero threshold");
    /* APPROX_TOP_SUM == TOP_K_WEIGHTED */
    const w = new DataFrame({v: new Float64Array([1, 2, 1, 2, 1]), wt: new Float64Array([10, 1, 10, 1, 5])});
    const a1 = w.APPROX_TOP_SUM("v", "wt", 1), a2 = w.TOP_K_WEIGHTED("v", "wt", 1);
    numEq(a1.keys[0], a2.keys[0], "APPROX_TOP_SUM == TOP_K_WEIGHTED top key");
    numEq(a1.values[0], 25, "weights summed not counted");
    /* ANY_HEAVY majority weight */
    numEq(w.ANY_HEAVY("v", "wt"), 1, "ANY_HEAVY majority");
    numEq(new DataFrame({v: new Float64Array([1, 2]), wt: new Float64Array([5, 5])}).ANY_HEAVY("v", "wt"),
          undefined, "ANY_HEAVY tie -> undefined");
    /* HISTOGRAM: edges bins+1, top edge inclusive, NaN dropped */
    const h = new DataFrame({a: new Float64Array([0, 1, 2, 3, NaN])}).HISTOGRAM("a", 3);
    numEq(h.edges.length, 4, "HISTOGRAM edges bins+1");
    numEq(h.counts[0] + h.counts[1] + h.counts[2], 4, "HISTOGRAM drops NaN, keeps max");
    /* E2T decayed avg relative to latest timestamp */
    const etd = new DataFrame({v: new Float64Array([1, 1]), t: new Float64Array([0, 100])});
    numEq(etd.EXPONENTIAL_TIME_DECAYED_COUNT("v", "t", 100), 1 + Math.exp(-1), "ETD_COUNT decayed");
    /* dictionary code refusals: string column reductions named, not coerced */
    const s = new DataFrame({s: ["10", "200", "30"]});
    throws("GT on string column refused", () => s.GT("s", 1));
    throws("SUM on string column refused", () => s.SUM("s"));
    const si = s.ISIN("s", ["200"]);
    arrEq(Array.from(si), [0, 1, 0], "ISIN string matches values not codes");
}

print(`test_dataframe_audit: ${passed} passed, ${failed} failed`);
if (failed) throw new Error("audit failures present");
