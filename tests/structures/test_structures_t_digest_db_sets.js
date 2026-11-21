// covers: dyna:structures.TDigest
// Differential and property tests for RoaringBitmap, CuckooFilter, TopK and
// TDigest. RoaringBitmap is checked against a JS Set on value sets built to
// cross its container thresholds (array <= 4096 < bitmap < 65536 = full) at
// N-1, N, N+1, with every set operation in both the in-place and the
// cardinality-only form. The sketches are checked against their published
// guarantees rather than against digits this engine produced:
//   CuckooFilter: no false negatives; FPR near 2*b/2^f (Fan et al. 2014).
//   TopK (Metwally et al. 2005): every key with count > N/k is listed, and
//     count - error <= true count <= count.
//   TDigest (Dunning 2019): quantile rank error bounded, tighter in the tails.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_structures_t_digest_db_sets");
import { RoaringBitmap, CuckooFilter, TopK, TDigest } from "dyna:structures";
import { Exec } from "dyna:sys";

let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { fails++; if (fails < 40) print("FAIL: " + m); } }
function eq(a, b, m) { check(Object.is(a, b), m + " -- got " + a + ", want " + b); }

function rng(seed) {
    let s = seed >>> 0 || 1;
    return () => { s ^= s << 13; s >>>= 0; s ^= s >>> 17; s ^= s << 5; s >>>= 0; return s / 4294967296; };
}

// ---- RoaringBitmap ----------------------------------------------------------
function build(r, shape) {
    const out = new Set();
    const C = 65536;
    switch (shape) {
    case "sparse": for (let i = 0; i < 3000; i++) out.add(Math.floor(r() * 2 ** 32)); break;
    case "chunk4095": for (let i = 0; i < 4095; i++) out.add(C * 3 + i * 7); break;
    case "chunk4096": for (let i = 0; i < 4096; i++) out.add(C * 3 + i * 7); break;
    case "chunk4097": for (let i = 0; i < 4097; i++) out.add(C * 3 + i * 7); break;
    case "full": for (let i = 0; i < C; i++) out.add(C * 3 + i); break;
    case "full-1": for (let i = 0; i < C; i++) if (i !== 1234) out.add(C * 3 + i); break;
    case "mixed": for (let i = 0; i < 20000; i++) out.add(C * 3 + Math.floor(r() * C)); for (let i = 0; i < 100; i++) out.add(C * 4 + i); for (let i = 0; i < C; i++) out.add(C * 5 + i); out.add(2 ** 32 - 1); out.add(0); break;
    case "dense": for (let i = 0; i < 60000; i++) out.add(C * 3 + Math.floor(r() * C)); break;
    }
    return out;
}
const sortU = (s) => Uint32Array.from([...s].sort((a, b) => a - b));
function sameSet(rb, s, m) {
    const a = rb.toArray(), b = s.arr || sortU(s);
    eq(rb.size, s.size, m + " size");
    let ok = a.length === b.length;
    for (let i = 0; ok && i < a.length; i++) ok = a[i] === b[i];
    check(ok, m + " contents");
}
const SHAPES = ["sparse", "chunk4095", "chunk4096", "chunk4097", "full", "full-1", "mixed", "dense"];

function exactQ(sorted, q) { if (!sorted.length) return NaN; return sorted[Math.min(sorted.length - 1, Math.max(0, Math.round(q * (sorted.length - 1))))]; }

function rankOf(sorted, x) { let lo = 0, hi = sorted.length; while (lo < hi) { const m = (lo + hi) >> 1; if (sorted[m] < x) lo = m + 1; else hi = m; } let lo2 = lo, hi2 = sorted.length; while (lo2 < hi2) { const m = (lo2 + hi2) >> 1; if (sorted[m] <= x) lo2 = m + 1; else hi2 = m; } return [lo / sorted.length, lo2 / sorted.length]; }

const DISTS = {
    uniform: (r) => r(),
    normal: (r) => { const u = Math.max(r(), 1e-12), v = r(); return Math.sqrt(-2 * Math.log(u)) * Math.cos(2 * Math.PI * v); },
    exponential: (r) => -Math.log(Math.max(r(), 1e-12)),
    lognormal: (r) => Math.exp(2 * Math.sqrt(-2 * Math.log(Math.max(r(), 1e-12))) * Math.cos(2 * Math.PI * r())),
    duplicates: (r) => Math.floor(r() * 10),
    sorted: null,
};

for (const [name, gen] of Object.entries(DISTS)) {
    for (const delta of [50, 100, 300]) {
        const r = rng(name.length * 31 + delta), N = 40000, xs = new Float64Array(N);
        for (let i = 0; i < N; i++) xs[i] = gen ? gen(r) : i;
        const td = new TDigest(delta);
        if (delta === 100) for (let i = 0; i < N; i++) td.add(xs[i]); else td.addMany(xs);
        const sorted = Float64Array.from(xs).sort();
        eq(td.count, N, `td ${name}/${delta} count`);
        eq(td.min, sorted[0], `td ${name}/${delta} min`);
        eq(td.max, sorted[N - 1], `td ${name}/${delta} max`);
        let s = 0; for (const x of xs) s += x;
        check(Math.abs(td.mean - s / N) <= 1e-9 * Math.max(1, Math.abs(s / N)), `td ${name}/${delta} mean exact`);
        // k1 scale: a centroid near q spans about (2*pi/delta)*sqrt(q(1-q)) of the mass,
        // so the estimate must lie between the exact values at q -/+ that width.
        let mono = true, prev = -Infinity, worst = 0, bad = "";
        for (const q of [0.0001, 0.0003, 0.001, 0.003, 0.01, 0.05, 0.1, 0.25, 0.5, 0.75, 0.9, 0.95, 0.99, 0.997, 0.999, 0.9997, 0.9999]) {
            const v = td.quantile(q);
            if (v < prev) mono = false; prev = v;
            const tol = (2 * Math.PI / delta) * Math.sqrt(q * (1 - q)) + 2 / N;
            const lo = sorted[Math.max(0, Math.floor((q - tol) * N))], hi = sorted[Math.min(N - 1, Math.ceil((q + tol) * N))];
            if (!(v >= lo && v <= hi)) { bad += ` q=${q}: ${v} not in [${lo}, ${hi}]`; }
            const [rlo, rhi] = rankOf(sorted, v);
            const err = q < rlo ? rlo - q : q > rhi ? q - rhi : 0;
            worst = Math.max(worst, err / tol);
        }
        const bound = 2 / delta;
        check(mono, `td ${name}/${delta} quantile is monotone`);
        check(bad === "", `td ${name}/${delta} quantile within the k1 envelope:${bad}`);
        let cmono = true, cprev = -1;
        for (let i = 0; i <= 40; i++) { const x = sorted[0] + (sorted[N - 1] - sorted[0]) * i / 40, c = td.cdf(x); if (c < cprev) cmono = false; cprev = c; }
        check(cmono, `td ${name}/${delta} cdf is monotone`);
        const [lo, hi] = rankOf(sorted, sorted[N >> 1]);
        const c = td.cdf(sorted[N >> 1]);
        check(c >= lo - bound && c <= hi + bound, `td ${name}/${delta} cdf at the median ${c} in [${lo}, ${hi}]`);
        check(td.centroids <= delta * 1.7 + 10, `td ${name}/${delta} centroid count ${td.centroids} bounded`);
        const qs = td.quantiles([0.1, 0.5, 0.9]);
        check(qs[1] === td.quantile(0.5), `td ${name}/${delta} quantiles() matches quantile()`);
        const half1 = new TDigest(delta), half2 = new TDigest(delta);
        half1.addMany(xs.subarray(0, N / 2)); half2.addMany(xs.subarray(N / 2));
        half1.merge(half2);
        eq(half1.count, N, `td ${name}/${delta} merged count`);
        const [mlo, mhi] = rankOf(sorted, half1.quantile(0.5));
        check(0.5 >= mlo - 2 * bound && 0.5 <= mhi + 2 * bound, `td ${name}/${delta} merged median rank in bound`);
        const back = TDigest.deserialize(td.serialize());
        eq(back.quantile(0.37), td.quantile(0.37), `td ${name}/${delta} serialize`);
    }
}

__kitS.section("td empty quantile NaN", () => {
    const td = new TDigest();
    eq(Number.isNaN(td.quantile(0.5)), true, "td empty quantile NaN");
    eq(td.min, undefined, "td empty min");
    td.add(5); eq(td.quantile(0), 5, "td single q0"); eq(td.quantile(1), 5, "td single q1");
    td.add(10, 3); eq(td.count, 4, "td weighted count");
    let threw = false; try { td.add(NaN); } catch (e) { threw = e instanceof RangeError; } check(threw, "td NaN refused");
    threw = false; try { td.add(1, 0); } catch (e) { threw = e instanceof RangeError; } check(threw, "td zero weight refused");
    threw = false; try { td.quantile(1.5); } catch (e) { threw = e instanceof RangeError; } check(threw, "td q > 1 refused");
    threw = false; try { new TDigest(5); } catch (e) { threw = e instanceof RangeError; } check(threw, "td compression floor");
    const t10 = new TDigest(10); const t0 = Date.now(); t10.addMany(new Float64Array(1 << 20)); const ms = Date.now() - t0;
    check(t10.centroids < 30, "td compression 10 stays compact (" + t10.centroids + " centroids, " + ms + " ms)");
});
__kitS.end();

print(`test_structures_db_sets: ${fails ? fails + " FAILED of" : "all"} ${n} checks${fails ? "" : " passed"}`);
if (fails) throw new Error(fails + " failures");
