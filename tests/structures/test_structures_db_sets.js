// flags: --std
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
{
    const r = rng(3);
    const sets = SHAPES.map(sh => build(r, sh));
    const sortedAll = sets.map(sortU);
    const bitmaps = sortedAll.map(a => new RoaringBitmap(a));
    for (let i = 0; i < SHAPES.length; i++) {
        const s = sets[i], rb = new RoaringBitmap(sortU(s));
        sameSet(rb, s, `rb ${SHAPES[i]} build`);
        sameSet(new RoaringBitmap([...s].reverse()), s, `rb ${SHAPES[i]} build from unsorted array`);
        sameSet(RoaringBitmap.deserialize(rb.serialize()), s, `rb ${SHAPES[i]} serialize`);
        const sorted = sortU(s);
        for (let t = 0; t < 30; t++) {
            const x = r() < 0.5 && sorted.length ? sorted[Math.floor(r() * sorted.length)] + (r() < 0.5 ? 0 : 1) : Math.floor(r() * 2 ** 32);
            eq(rb.has(x), s.has(x), `rb ${SHAPES[i]} has`);
            let rk = 0; { let lo = 0, hi = sorted.length; while (lo < hi) { const mid = (lo + hi) >> 1; if (sorted[mid] <= x) lo = mid + 1; else hi = mid; } rk = lo; }
            eq(rb.rank(x), rk, `rb ${SHAPES[i]} rank(${x})`);
            const k = Math.floor(r() * (sorted.length + 2));
            eq(rb.select(k), sorted[k], `rb ${SHAPES[i]} select(${k})`);
        }
        eq(rb.min(), sorted[0], `rb ${SHAPES[i]} min`);
        eq(rb.max(), sorted[sorted.length - 1], `rb ${SHAPES[i]} max`);
        for (let j = 0; j < SHAPES.length; j++) {
            const ob = bitmaps[j];
            const A = sortedAll[i], B = sortedAll[j], I = [], U = [], X = [], D = [];
            for (let p = 0, q = 0; p < A.length || q < B.length;) {
                if (q >= B.length || (p < A.length && A[p] < B[q])) { U.push(A[p]); X.push(A[p]); D.push(A[p]); p++; }
                else if (p >= A.length || B[q] < A[p]) { U.push(B[q]); X.push(B[q]); q++; }
                else { I.push(A[p]); U.push(A[p]); p++; q++; }
            }
            const inter = { size: I.length, arr: I }, uni = { size: U.length, arr: U }, xr = { size: X.length, arr: X }, dif = { size: D.length, arr: D };
            const tag = `rb ${SHAPES[i]} op ${SHAPES[j]}`;
            sameSet(rb.clone().and(ob), inter, tag + " and");
            sameSet(rb.clone().or(ob), uni, tag + " or");
            sameSet(rb.clone().xor(ob), xr, tag + " xor");
            sameSet(rb.clone().andNot(ob), dif, tag + " andNot");
            eq(rb.andCardinality(ob), inter.size, tag + " andCardinality");
            eq(rb.orCardinality(ob), uni.size, tag + " orCardinality");
            eq(rb.xorCardinality(ob), xr.size, tag + " xorCardinality");
            eq(rb.andNotCardinality(ob), dif.size, tag + " andNotCardinality");
            eq(rb.intersects(ob), inter.size > 0, tag + " intersects");
            eq(rb.isSubsetOf(ob), inter.size === s.size, tag + " isSubsetOf");
            eq(rb.equals(ob), i === j, tag + " equals");
            eq(rb.jaccard(ob), uni.size ? inter.size / uni.size : 1, tag + " jaccard");
        }
        const self = rb.clone(); self.and(self); sameSet(self, s, `rb ${SHAPES[i]} and self`);
        self.xor(self); eq(self.size, 0, `rb ${SHAPES[i]} xor self empties`);
    }
}
{
    const r = rng(9), rb = new RoaringBitmap(), s = new Set();
    for (let step = 0; step < 1500; step++) {
        const op = r(), base = 65536 * Math.floor(r() * 4);
        if (op < 0.5) { const x = base + Math.floor(r() * 9000); rb.add(x); s.add(x); }
        else if (op < 0.75) { const x = base + Math.floor(r() * 9000); eq(rb.delete(x), s.delete(x), "rb churn delete"); }
        else if (op < 0.85) { const lo = base + Math.floor(r() * 70000), hi = lo + Math.floor(r() * 80000); rb.addRange(lo, hi); for (let x = lo; x < hi; x++) s.add(x); }
        else if (op < 0.95) { const lo = base + Math.floor(r() * 70000), hi = lo + Math.floor(r() * 80000); rb.removeRange(lo, hi); for (const x of [...s]) if (x >= lo && x < hi) s.delete(x); }
        else { const lo = Math.floor(r() * 300000), hi = lo + Math.floor(r() * 100000); let c = 0; for (const x of s) if (x >= lo && x < hi) c++; eq(rb.rangeCardinality(lo, hi), c, "rb churn rangeCardinality"); }
        if (step % 300 === 0) sameSet(rb, s, `rb churn @${step}`);
    }
    sameSet(rb, s, "rb churn final");
    sameSet(RoaringBitmap.deserialize(rb.serialize()), s, "rb churn serialize");
}
{
    const rb = new RoaringBitmap();
    rb.addRange(0, 2 ** 32);
    eq(rb.size, 2 ** 32, "rb full universe size");
    check(rb.bytes < 4e6, "rb full universe stays small (" + rb.bytes + " bytes)");
    eq(rb.has(2 ** 32 - 1), true, "rb full has max");
    eq(rb.rank(2 ** 32 - 1), 2 ** 32, "rb full rank");
    eq(rb.select(2 ** 32 - 1), 2 ** 32 - 1, "rb full select last");
    let threw = false; try { rb.toArray(); } catch (e) { threw = e instanceof RangeError; } check(threw, "rb toArray refuses 2^32 values");
    rb.delete(5); eq(rb.size, 2 ** 32 - 1, "rb delete from a full container");
    eq(rb.has(5), false, "rb deleted from full");
    rb.removeRange(0, 2 ** 32); eq(rb.isEmpty(), true, "rb removeRange whole universe");
    threw = false; try { rb.add(2 ** 32); } catch (e) { threw = e instanceof RangeError; } check(threw, "rb 2^32 refused");
    threw = false; try { rb.add(-1); } catch (e) { threw = e instanceof RangeError; } check(threw, "rb -1 refused");
    threw = false; try { rb.add(1.5); } catch (e) { threw = e instanceof RangeError; } check(threw, "rb 1.5 refused");
    threw = false; try { new RoaringBitmap(new Int32Array([1, -2])); } catch (e) { threw = e instanceof RangeError; } check(threw, "rb negative Int32Array refused");
    eq([...new RoaringBitmap([3, 1, 2])].join(), "1,2,3", "rb iterator ascending");
    eq(new RoaringBitmap().min(), undefined, "rb empty min");
    eq(new RoaringBitmap().jaccard(new RoaringBitmap()), 1, "rb empty jaccard");
}

// ---- CuckooFilter -----------------------------------------------------------
{
    const N = 50000, cf = new CuckooFilter(N);
    let added = 0;
    for (let i = 0; i < N; i++) if (cf.add("key:" + i)) added++;
    eq(added, N, "cuckoo: every key fits at the requested capacity");
    let fn = 0; for (let i = 0; i < N; i++) if (!cf.mayContain("key:" + i)) fn++;
    eq(fn, 0, "cuckoo: no false negatives");
    let fp = 0; const T = 200000; for (let i = 0; i < T; i++) if (cf.mayContain("other:" + i)) fp++;
    check(fp / T < 0.0005, "cuckoo: false positive rate " + (fp / T) + " under 0.05% (theory ~0.012%)");
    let del = 0; for (let i = 0; i < N; i += 2) if (cf.delete("key:" + i)) del++;
    eq(del, N / 2, "cuckoo: deletes of added keys succeed");
    fn = 0; for (let i = 1; i < N; i += 2) if (!cf.mayContain("key:" + i)) fn++;
    eq(fn, 0, "cuckoo: survivors still present after deletes");
    let gone = 0; for (let i = 0; i < N; i += 2) if (!cf.mayContain("key:" + i)) gone++;
    check(gone > N / 2 * 0.99, "cuckoo: deleted keys read absent (" + gone + ")");
    eq(cf.size, N / 2, "cuckoo: size after deletes");
    const back = CuckooFilter.deserialize(cf.serialize());
    fn = 0; for (let i = 1; i < N; i += 2) if (!back.mayContain("key:" + i)) fn++;
    eq(fn, 0, "cuckoo: serialize keeps every key");
    const small = new CuckooFilter(8); let ok = 0;
    for (let i = 0; i < 1000; i++) if (small.add("s" + i)) ok++;
    check(ok < 1000 && ok >= 8, "cuckoo: a full filter refuses (" + ok + " accepted)");
    let lost = 0; for (let i = 0; i < 1000; i++) if (!small.mayContain("s" + i) && i < ok) lost++;
    // every key that add() accepted must still be found, including the victim slot
    const acc = new CuckooFilter(8), keys = [];
    for (let i = 0; i < 1000; i++) if (acc.add("v" + i)) keys.push("v" + i);
    check(keys.every(k => acc.mayContain(k)), "cuckoo: accepted keys survive the eviction chain");
    let threw = false; try { new CuckooFilter(2 ** 27); } catch (e) { threw = e instanceof RangeError; } check(threw, "cuckoo: capacity cap");
}
{
    // the hash is fixed: a filter written by another process answers the same
    const code = `import { CuckooFilter, BloomFilter } from "dyna:structures"; import * as std from "std";
const c = new CuckooFilter(1000); for (let i = 0; i < 500; i++) c.add("p" + i);
const b = new BloomFilter(1 << 14, 5); for (let i = 0; i < 500; i++) b.add("p" + i);
const out = [...c.serialize()].join(",") + "|" + [...b.serialize()].join(","); std.out.puts(out);`;
    const child = Exec("./dynajs", ["--std", "-m", "-e", code]);
    const [cs, bs] = child.stdout.split("|");
    const cf = CuckooFilter.deserialize(new Uint8Array(cs.split(",").map(Number)));
    const bf = (await import("dyna:structures")).BloomFilter.deserialize(new Uint8Array(bs.split(",").map(Number)));
    let cmiss = 0, bmiss = 0; for (let i = 0; i < 500; i++) { if (!cf.mayContain("p" + i)) cmiss++; if (!bf.mayContain("p" + i)) bmiss++; }
    eq(cmiss, 0, "cuckoo: a filter serialized by another process has no false negatives");
    eq(bmiss, 0, "bloom: a filter serialized by another process has no false negatives");
}

// ---- TopK -------------------------------------------------------------------
for (const [seed, k, skew] of [[1, 10, 1.1], [2, 50, 1.3], [3, 5, 0.9], [4, 100, 1.0]]) {
    const r = rng(seed), tk = new TopK(k), truth = new Map();
    const V = 2000, cdf = [];
    let acc = 0; for (let i = 1; i <= V; i++) { acc += 1 / i ** skew; cdf.push(acc); }
    let N = 0;
    for (let t = 0; t < 60000; t++) {
        const u = r() * acc; let lo = 0, hi = V - 1; while (lo < hi) { const m = (lo + hi) >> 1; if (cdf[m] < u) lo = m + 1; else hi = m; }
        const key = "w" + lo, c = r() < 0.1 ? 3 : 1;
        tk.add(key, c); truth.set(key, (truth.get(key) || 0) + c); N += c;
    }
    eq(tk.total, N, `topk[${seed}] total`);
    const listed = new Map(tk.list().map(e => [e[0], e]));
    eq(listed.size, k, `topk[${seed}] k counters in use`);
    let guaranteeOK = true, boundsOK = true;
    for (const [key, c] of truth) if (c > N / k && !listed.has(key)) guaranteeOK = false;
    for (const [key, [, est, err]] of listed) { const t = truth.get(key) || 0; if (!(est >= t && est - err <= t)) boundsOK = false; }
    check(guaranteeOK, `topk[${seed}] every key above N/k is listed`);
    check(boundsOK, `topk[${seed}] count - error <= truth <= count for every listed key`);
    const l = tk.list(); check(l.every((e, i) => i === 0 || l[i - 1][1] >= e[1]), `topk[${seed}] list is sorted by count`);
    const back = TopK.deserialize(tk.serialize());
    eq(JSON.stringify(back.list()), JSON.stringify(tk.list()), `topk[${seed}] serialize`);
}
{
    const tk = new TopK(2);
    eq(tk.add("a"), undefined, "topk: first add evicts nothing");
    tk.add("b", 5);
    eq(tk.add("c"), "a", "topk: the minimum counter is evicted and returned");
    eq(tk.count("c"), 2, "topk: newcomer inherits min + n");
    eq(tk.error("c"), 1, "topk: error is the inherited minimum");
    eq(tk.count("zzz"), 0, "topk: untracked count is 0");
}

// ---- TDigest ----------------------------------------------------------------
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
{
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
}

print(`test_structures_db_sets: ${fails ? fails + " FAILED of" : "all"} ${n} checks${fails ? "" : " passed"}`);
if (fails) throw new Error(fails + " failures");
