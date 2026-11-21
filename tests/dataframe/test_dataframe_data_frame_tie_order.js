// covers: dyna:dataframe.DataFrame
// Owner decision D9 (issue F26): every DataFrame verb whose result depends on how equal keys or timestamps are ordered
// uses ONE specified order, ties keep input row order, at every row count, so a kernel that switches algorithm at a size
// threshold can never change a result, not even the last bit of a float sum. Each verb is checked against a JS oracle
// (a stable sort plus a sequential sum in the C summation order) at counts straddling every switch the kernels have
// (24/25 insertion, 63/64 small radix, 511/512 DELTA_SUM_TIMESTAMP radix, 2047/2048/2049 the old quicksort limit),
// counted AFTER mask and NaN filtering, with masked rows, NaN timestamps, -0/0 keys and non-integer values whose last
// bits move when the order of a tie moves. A sensitivity check proves the data would notice a reversed tie order.
//   DELTA_SUM_TIMESTAMP: index-tiebreak quicksort below 512 rows, stable LSD radix from 512, no sort when in order.
//   TOP_K_WEIGHTED / QUANTILE_EXACT_WEIGHTED: per-key weight sums in row order (-0 and 0 one key), then distinct keys.
//   t-digest verbs (QUANTILE_TDIGEST_WEIGHTED, APPROX_PERCENTILE, QUANTILES_TDIGEST): the buffer used libc qsort below
//   64 records (pivot order for equal means, so the same rows in a tie-preserving shuffle gave different quantiles); it
//   now takes a stable shell sort of (key, row) there. The JS port of the digest is exact except where the compiler fuses a
//   multiply-add (last ulp), so it is compared at 1e-12 relative; bit-identity is checked by a tie-preserving shuffle with
//   dyadic weights (the total is a row-order sum, so only exactly-summable weights make it shuffle-invariant).
//   GROUP_ARRAY_SORTED: below 64 values a qsort left -0 and 0 in pivot order; now Float64Array.prototype.sort order
//   (-0 before 0, NaN last) at every size, the order the radix already gave from 64 (a shell sort of the bit keys below).
//   RANGE_AGG: equal starts -0/0 came out in qsort order; -0 and 0 are now one bound reported as 0 (like TOP_K_WEIGHTED).
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_dataframe_data_frame_tie_order");
import { DataFrame } from "dyna:dataframe";

let checks = 0, fails = 0;
const show = (x) => (Object.is(x, -0) ? "-0" : String(x));
function eq(got, want, what) {
    checks++;
    if (!Object.is(got, want)) { fails++; print("FAIL: " + what + ": got " + show(got) + ", want " + show(want)); }
}
function near(got, want, what) {
    checks++;
    const ok = Object.is(got, want) || (Number.isFinite(want) && Math.abs(got - want) <= 1e-12 * Math.max(1, Math.abs(want)));
    if (!ok) { fails++; print("FAIL: " + what + ": got " + show(got) + ", want " + show(want)); }
}
function arr(got, want, what) {
    checks++;
    let bad = got.length !== want.length ? "length " + got.length + " vs " + want.length : "";
    for (let i = 0; !bad && i < want.length; i++) if (!Object.is(got[i], want[i])) bad = "at " + i + ": " + show(got[i]) + " vs " + show(want[i]);
    if (bad) { fails++; print("FAIL: " + what + ": " + bad); }
}
let seed = 0x1F2E3D4C;
const rnd = () => { seed ^= seed << 13; seed ^= seed >>> 17; seed ^= seed << 5; seed >>>= 0; return seed / 4294967296; };
const z = (x) => (x === 0 ? 0 : x);
const fval = () => (rnd() - 0.3) * Math.pow(10, rnd() * 6 - 3);

function tiePerm(keys) {
    const n = keys.length, groups = new Map(), at = new Map(), out = new Array(n);
    const key = (i) => (Number.isNaN(keys[i]) ? "nan" : z(keys[i]));
    for (let i = 0; i < n; i++) { const k = key(i); if (!groups.has(k)) groups.set(k, []); groups.get(k).push(i); }
    const slots = Array.from({ length: n }, (_, i) => i);
    for (let i = n - 1; i > 0; i--) { const j = (rnd() * (i + 1)) | 0; const x = slots[i]; slots[i] = slots[j]; slots[j] = x; }
    for (let p = 0; p < n; p++) { const k = key(slots[p]), c = at.get(k) || 0; out[p] = groups.get(k)[c]; at.set(k, c + 1); }
    return out;
}
const permute = (a, p) => { const o = new a.constructor(p.length); for (let i = 0; i < p.length; i++) o[i] = a[p[i]]; return o; };

function framed(valid, mk) {
    const rows = [], extra = Math.max(1, valid >> 3);
    let left = valid, junk = extra;
    while (left || junk) {
        if (junk && (!left || rnd() < extra / (valid + extra))) { rows.push({ junk: true }); junk--; } else { rows.push(mk()); left--; }
    }
    return rows;
}

const SIZES = [1, 2, 3, 24, 25, 63, 64, 65, 511, 512, 513, 2047, 2048, 2049, 4100];

function deltaOracle(t, v, mask, reverseTies) {
    const ord = [];
    for (let i = 0; i < t.length; i++) if ((!mask || mask[i]) && !Number.isNaN(t[i])) ord.push(i);
    ord.sort((a, b) => t[a] - t[b] || (reverseTies ? b - a : 0));
    let s = 0;
    for (let i = 1; i < ord.length; i++) if (v[ord[i]] > v[ord[i - 1]]) s += v[ord[i]] - v[ord[i - 1]];
    return s;
}

__kitS.section("DELTA_SUM_TIMESTAMP: equal timestamps keep row order at every filtered count, bit-identical to a stable-sort oracle", () => {
    let sensitive = 0;
    for (const valid of SIZES) for (const shape of ["ties", "zeros", "sorted-ties", "distinct"]) {
        const span = Math.max(2, valid >> 2);
        let tick = 0;
        const rows = framed(valid, () => {
            const t = shape === "distinct" ? rnd() * 1e6 : shape === "sorted-ties" ? Math.floor(tick++ / 3) : shape === "zeros" ? (rnd() < 0.5 ? (rnd() < 0.5 ? -0 : 0) : (rnd() * 3) | 0) : (rnd() * span) | 0;
            return { t, v: rnd() < 0.02 ? NaN : fval(), m: 1 };
        });
        for (const r of rows) if (r.junk) Object.assign(r, rnd() < 0.5 ? { t: NaN, v: fval(), m: 1 } : { t: (rnd() * span) | 0, v: fval(), m: 0 });
        const t = Float64Array.from(rows, (r) => r.t), v = Float64Array.from(rows, (r) => r.v), m = Uint8Array.from(rows, (r) => r.m);
        const want = deltaOracle(t, v, m, false);
        const df = new DataFrame({ v, t });
        eq(df.DELTA_SUM_TIMESTAMP("v", "t", m), want, shape + " valid=" + valid);
        if (!Object.is(deltaOracle(t, v, m, true), want)) sensitive++;
        const p = tiePerm(Float64Array.from(rows, (r) => (r.m ? r.t : NaN)));
        eq(new DataFrame({ v: permute(v, p), t: permute(t, p) }).DELTA_SUM_TIMESTAMP("v", "t", permute(m, p)), want, shape + " valid=" + valid + " tie-preserving shuffle");
        const um = Float64Array.from(rows, (r) => r.t);
        eq(df.DELTA_SUM_TIMESTAMP("v", "t"), deltaOracle(um, v, null, false), shape + " valid~" + valid + " unmasked");
    }
    checks++;
    if (sensitive < 10) { fails++; print("FAIL: only " + sensitive + " cases notice a reversed tie order; the data is not sensitive"); }
});

function weightedOracle(k, w, mask) {
    const acc = new Map();
    for (let i = 0; i < k.length; i++) {
        if ((mask && !mask[i]) || Number.isNaN(k[i])) continue;
        const key = z(k[i]);
        acc.set(key, (acc.has(key) ? acc.get(key) : 0) + (w ? w[i] : 1));
    }
    return [...acc].sort((a, b) => a[0] - b[0]);
}

__kitS.section("TOP_K_WEIGHTED and QUANTILE_EXACT_WEIGHTED: each key's weights are summed in row order at every size", () => {
    let sensitive = 0;
    for (const valid of SIZES) for (const nk of [3, 24, 25, 64, 300]) {
        const rows = framed(valid, () => ({ k: rnd() < 0.1 ? (rnd() < 0.5 ? -0 : 0) : ((rnd() * nk) | 0) - (nk >> 1), w: Math.abs(fval()) + 1e-3, m: 1 }));
        for (const r of rows) if (r.junk) Object.assign(r, rnd() < 0.5 ? { k: NaN, w: 1, m: 1 } : { k: 1, w: 5, m: 0 });
        const k = Float64Array.from(rows, (r) => r.k), w = Float64Array.from(rows, (r) => r.w), m = Uint8Array.from(rows, (r) => r.m);
        const sums = weightedOracle(k, w, m);
        const top = sums.slice().sort((a, b) => b[1] - a[1] || a[0] - b[0]);
        const df = new DataFrame({ k, w });
        const got = df.TOP_K_WEIGHTED("k", "w", 65536, m);
        arr(got.keys, top.map((e) => e[0]), "TOP_K keys valid=" + valid + " nk=" + nk);
        arr(got.values, top.map((e) => e[1]), "TOP_K weights valid=" + valid + " nk=" + nk);
        const rev = new Map();
        for (let i = k.length - 1; i >= 0; i--) if (m[i] && !Number.isNaN(k[i])) rev.set(z(k[i]), (rev.get(z(k[i])) || 0) + w[i]);
        if (sums.some(([key, s]) => !Object.is(rev.get(key), s))) sensitive++;
        let total = 0;
        for (const e of sums) total += e[1];
        for (const q of [0, 0.1, 0.25, 0.5, 0.75, 0.9, 1]) {
            let want;
            if (sums.length && total > 0) {
                const target = q * total;
                let run = 0;
                want = sums[sums.length - 1][0];
                for (const [key, s] of sums) { run += s; if (run >= target) { want = key; break; } }
            }
            eq(df.QUANTILE_EXACT_WEIGHTED("k", "w", q, m), want, "QEW q=" + q + " valid=" + valid + " nk=" + nk);
        }
        const p = tiePerm(Float64Array.from(rows, (r) => (r.m ? r.k : NaN)));
        const g2 = new DataFrame({ k: permute(k, p), w: permute(w, p) }).TOP_K_WEIGHTED("k", "w", 65536, permute(m, p));
        arr(g2.values, top.map((e) => e[1]), "TOP_K tie-preserving shuffle valid=" + valid + " nk=" + nk);
        const c = df.TOP_K_WEIGHTED("k", undefined, 65536, m), ct = weightedOracle(k, null, m).sort((a, b) => b[1] - a[1] || a[0] - b[0]);
        arr(c.keys, ct.map((e) => e[0]), "TOP_K counts valid=" + valid + " nk=" + nk);
    }
    checks++;
    if (sensitive < 10) { fails++; print("FAIL: only " + sensitive + " cases notice a reversed accumulation order"); }
});

const NORM = 100 / (2 * 3.14159265358979323846), HALF_PI = 1.5707963267948966;
const tdK = (q) => (q <= 0 ? -NORM * HALF_PI : q >= 1 ? NORM * HALF_PI : NORM * Math.asin(2 * q - 1));
const tdQ = (k) => { const t = k / NORM; return t <= -HALF_PI ? 0 : t >= HALF_PI ? 1 : (Math.sin(t) + 1) / 2; };
function tdigest(vals, ws, mask) {
    const t = { main: [], buf: [], total: 0, vmin: Infinity, vmax: -Infinity };
    const mean = (ma, wa, mb, wb) => (ma === mb ? ma : !Number.isFinite(ma) || !Number.isFinite(mb) ? (wa >= wb ? ma : mb) : ma + (mb - ma) * (wb / (wa + wb)));
    const flush = () => {
        if (!t.buf.length) return;
        const b = t.buf.map((c, i) => [c, i]).sort((x, y) => z(x[0].m) - z(y[0].m) || x[1] - y[1]).map((e) => e[0]);
        const mg = [];
        let i = 0, j = 0;
        while (i < t.main.length || j < b.length) mg.push(j >= b.length || (i < t.main.length && t.main[i].m <= b[j].m) ? t.main[i++] : b[j++]);
        t.buf = [];
        let wsf = 0, lim = tdQ(tdK(0) + 1), cur = { ...mg[0] };
        const out = [];
        for (let x = 1; x < mg.length; x++) {
            if ((wsf + cur.w + mg[x].w) / t.total <= lim || out.length === 399) { cur.m = mean(cur.m, cur.w, mg[x].m, mg[x].w); cur.w += mg[x].w; }
            else { out.push(cur); wsf += cur.w; lim = tdQ(tdK(wsf / t.total) + 1); cur = { ...mg[x] }; }
        }
        out.push(cur);
        t.main = out;
    };
    for (let i = 0; i < vals.length; i++) {
        const v = vals[i], w = ws ? ws[i] : 1;
        if ((mask && !mask[i]) || v !== v || !(w > 0)) continue;
        if (v < t.vmin) t.vmin = v;
        if (v > t.vmax) t.vmax = v;
        t.total += w;
        t.buf.push({ m: v, w });
        if (t.buf.length === 1024) flush();
    }
    return (q) => {
        flush();
        if (!(t.total > 0)) return undefined;
        if (q <= 0) return t.vmin;
        if (q >= 1) return t.vmax;
        if (t.main.length === 1) return t.main[0].m;
        const lerp = (l, r, f) => { const d = r - l; let v = d === 0 || !Number.isFinite(d) ? l : l + f * d; if (v < t.vmin) v = t.vmin; if (v > t.vmax) v = t.vmax; return v; };
        const idx = q * t.total;
        let wsf = 0;
        for (let i = 0; i < t.main.length; i++) {
            const c = t.main[i], centre = wsf + c.w / 2;
            if (idx <= centre) {
                const cl = i === 0 ? 0 : wsf - t.main[i - 1].w / 2, sp = centre - cl;
                return lerp(i === 0 ? t.vmin : t.main[i - 1].m, c.m, sp > 0 ? (idx - cl) / sp : 0);
            }
            wsf += c.w;
        }
        const last = t.main[t.main.length - 1], cl = t.total - last.w / 2, sp = t.total - cl;
        return lerp(last.m, t.vmax, sp > 0 ? (idx - cl) / sp : 1);
    };
}

const TD_SIZES = [2, 5, 24, 25, 40, 63, 64, 65, 300, 1023, 1024, 1025, 2049];
const QS = [0.01, 0.1, 0.3, 0.5, 0.71, 0.9, 0.99];

__kitS.section("t-digest verbs: equal values enter the digest in row order (oracle port, 1e-12) at every size", () => {
    for (const n of TD_SIZES) for (let rep = 0; rep < 6; rep++) {
        const v = Float64Array.from({ length: n }, () => (rnd() < 0.1 ? (rnd() < 0.5 ? -0 : 0) : ((rnd() * 6) | 0) * 1.37 + (rnd() < 0.3 ? rnd() : 0)));
        const w = Float64Array.from({ length: n }, () => (rnd() < 0.05 ? 0 : 0.1 + rnd() * 3.7));
        const m = Uint8Array.from({ length: n }, () => (rnd() < 0.85 ? 1 : 0));
        const df = new DataFrame({ v, w });
        const dw = tdigest(v, w, m), du = tdigest(v, null, null);
        for (const q of QS) {
            near(df.QUANTILE_TDIGEST_WEIGHTED("v", "w", q, m), dw(q), "weighted n=" + n + " q=" + q);
            near(df.APPROX_PERCENTILE("v", q), du(q), "APPROX_PERCENTILE n=" + n + " q=" + q);
        }
        const qs = df.QUANTILES_TDIGEST("v", QS);
        QS.forEach((q, i) => near(qs[i], du(q), "QUANTILES_TDIGEST n=" + n + " q=" + q));
    }
});

__kitS.section("t-digest verbs: a tie-preserving shuffle gives bit-identical quantiles (one buffer flush, n <= 1024)", () => {
    for (const n of TD_SIZES.filter((x) => x <= 1024)) for (let rep = 0; rep < 8; rep++) {
        const v = Float64Array.from({ length: n }, () => (rnd() < 0.15 ? (rnd() < 0.5 ? -0 : 0) : ((rnd() * 5) | 0) * 1.5));
        const w = Float64Array.from({ length: n }, () => (1 + ((rnd() * 31) | 0)) / 8);
        const p = tiePerm(v), v2 = permute(v, p), w2 = permute(w, p);
        const a = new DataFrame({ v, w }), b = new DataFrame({ v: v2, w: w2 });
        for (const q of QS) {
            eq(b.QUANTILE_TDIGEST_WEIGHTED("v", "w", q), a.QUANTILE_TDIGEST_WEIGHTED("v", "w", q), "weighted shuffle n=" + n + " q=" + q);
            eq(b.APPROX_PERCENTILE("v", q), a.APPROX_PERCENTILE("v", q), "unweighted shuffle n=" + n + " q=" + q);
        }
    }
});

__kitS.section("t-digest verbs over only -0 and 0: the first row's zero leads, exactly as the oracle says", () => {
    for (const n of [2, 3, 10, 63, 64, 65, 500]) for (let rep = 0; rep < 6; rep++) {
        const v = Float64Array.from({ length: n }, () => (rnd() < 0.5 ? -0 : 0));
        const w = Float64Array.from({ length: n }, () => 0.5 + rnd());
        const df = new DataFrame({ v, w }), dw = tdigest(v, w, null), du = tdigest(v, null, null);
        for (const q of [0.2, 0.5, 0.8]) {
            eq(df.QUANTILE_TDIGEST_WEIGHTED("v", "w", q), dw(q), "zeros weighted n=" + n + " q=" + q);
            eq(df.APPROX_PERCENTILE("v", q), du(q), "zeros n=" + n + " q=" + q);
        }
    }
});

__kitS.section("GROUP_ARRAY_SORTED: Float64Array sort order (-0 before 0, NaN last) at every group size", () => {
    for (const sizes of [[1, 2, 5], [24, 25, 63], [64, 65], [63, 64, 65, 130], [200, 3, 64]]) for (let rep = 0; rep < 4; rep++) {
        const k = [], v = [];
        sizes.forEach((sz, g) => { for (let i = 0; i < sz; i++) { k.push(g * 7 + 2); v.push(rnd() < 0.4 ? (rnd() < 0.5 ? -0 : 0) : rnd() < 0.1 ? NaN : ((rnd() * 5) | 0) - 2 + (rnd() < 0.3 ? 0.25 : 0)); } });
        const p = Array.from(k, (_, i) => i);
        for (let i = p.length - 1; i > 0; i--) { const j = (rnd() * (i + 1)) | 0; const x = p[i]; p[i] = p[j]; p[j] = x; }
        const ki = Int32Array.from(p, (i) => k[i]), vf = Float64Array.from(p, (i) => v[i]);
        const got = new DataFrame({ k: ki, v: vf }).GROUP_ARRAY_SORTED("k", "v");
        got.keys.forEach((key, gi) => {
            const want = Float64Array.from(vf.filter((_, i) => ki[i] === key)).sort();
            arr(got.values[gi], want, "group " + key + " sizes=" + sizes);
        });
    }
});

__kitS.section("RANGE_AGG: -0 and 0 are one bound reported as 0, so the union does not depend on row order", () => {
    for (const valid of [1, 3, 24, 25, 63, 64, 65, 2047, 2048, 2049]) for (let rep = 0; rep < 3; rep++) {
        const lo = [], hi = [];
        for (let i = 0; i < valid; i++) {
            const r = rnd();
            if (r < 0.3) { lo.push(rnd() < 0.5 ? -0 : 0); hi.push(1 + ((rnd() * 4) | 0)); }
            else if (r < 0.45) { lo.push(-1 - ((rnd() * 4) | 0)); hi.push(rnd() < 0.5 ? -0 : 0); }
            else { const a = ((rnd() * valid * 3) | 0) + 5; lo.push(a); hi.push(a + 0.5 + rnd() * 4); }
        }
        lo.push(NaN, 4, 9); hi.push(3, 4, 8);
        const want = lo.map((l, i) => [l, hi[i]]).filter(([l, h]) => l === l && h === h && l < h).map(([l, h]) => [z(l), z(h)]).sort((a, b) => a[0] - b[0]);
        const ws = [], we = [];
        for (const [l, h] of want) { if (ws.length && l <= we[we.length - 1]) { if (h > we[we.length - 1]) we[we.length - 1] = h; } else { ws.push(l); we.push(h); } }
        for (let s = 0; s < 3; s++) {
            const p = Array.from(lo, (_, i) => i);
            if (s) for (let i = p.length - 1; i > 0; i--) { const j = (rnd() * (i + 1)) | 0; const x = p[i]; p[i] = p[j]; p[j] = x; }
            const got = new DataFrame({ lo: Float64Array.from(p, (i) => lo[i]), hi: Float64Array.from(p, (i) => hi[i]) }).RANGE_AGG("lo", "hi");
            arr(got.starts, ws, "starts valid=" + valid + " shuffle " + s);
            arr(got.ends, we, "ends valid=" + valid + " shuffle " + s);
        }
    }
    for (const [lo, hi] of [[[-5, -5], [-0, 0]], [[-5, -5], [0, -0]], [[-0, 0, 3], [2, 1, 4]], [[0, -0], [2, 1]]]) {
        const got = new DataFrame({ lo: Float64Array.from(lo), hi: Float64Array.from(hi) }).RANGE_AGG("lo", "hi");
        const s0 = Math.min(...lo.map(z)), e0 = Math.max(...hi.filter((h, i) => lo[i] <= 0).map(z));
        eq(got.starts[0], s0 === 0 ? 0 : s0, "fixed start lo=" + lo.map(show) + " hi=" + hi.map(show));
        eq(got.ends[0], e0 === 0 ? 0 : e0, "fixed end lo=" + lo.map(show) + " hi=" + hi.map(show));
    }
});

if (fails) throw new Error(fails + " of " + checks + " checks failed");
print("test_dataframe_data_frame_tie_order: " + checks + " checks passed");
__kitS.end();
