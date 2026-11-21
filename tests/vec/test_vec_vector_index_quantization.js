// timeout: 600
// covers: dyna:vec.VectorIndex
// quantization "sq8" and "binary" for VectorIndex (flat and hnsw, every metric, rerank on and off).
// Rerank hides kernel faults (a wrong step, offset or clamp still returns nearly right answers after f32 re-scoring),
// so the kernel is pinned with dyna.rerank false against a JS implementation of the documented rule: per-dimension
// lo/hi from the first batch, mid = lo + (hi - lo) / 2, one shared step s = max half-width / 127, codes
// clamp(floor((x - mid) / s + 0.5), -127, 127), the widening rule (cover the batch, +25% of the widened width on each
// side that grew, re-encode from decoded codes), integer l2 and the dot form with the per-vector sum mid*c; binary is
// the sign bit (> 0) and the Hamming distance. Flat must equal the oracle exactly (ids, f32 distances, ties by id).
// Rerank on is checked separately: flat with efSearch >= size equals the f32 index exactly, and every returned
// distance equals the f32 distance of that id. Also: max dims (65536) with extreme codes (the l2 sum exceeds int32),
// the candidate pool (the best min(max(efSearch, k) x dyna.rerankFactor, size) by the quantized distance are reranked;
// factor 12 for binary and 1 for sq8 by default; a flat oracle pins the exact pool, and a loaded index keeps the factor),
// recall@10 per metric x quantization x index x rerank against f32 flat on 4000 clustered and 4000 uniform vectors of
// 64 dims (printed; floors: sq8 reranked >= 0.95, sq8 candidate stage >= 0.90, binary reranked >= 0.9 at the defaults;
// 64 bits per vector rank vectors coarsely, so binary reranks 12x the beam: factor 8 measured 0.905 on clustered l2,
// factor 12 0.958; the factor-1 row is printed for comparison), filters (uint32 and 64-bit) and tombstones on the
// beam, the fallback and flat, searchBatch, save/load byte identity in copy and mmap mode for every combination, a
// loaded copy continuing in lockstep through an add that widens the sq8 range, forged records for the new sections,
// and option errors.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_vec_vector_index_quantization");
import { VectorIndex } from "dyna:vec";
import { RoaringBitmap } from "dyna:structures";
import { makeTempDir, removeAll, readBytes, writeFile } from "dyna:file";
import { CRC32C } from "dyna:hash";

let pass = 0, fail = 0;
const ok = (c, w, d) => { if (c) pass++; else { fail++; print("  FAIL  " + w + (d ? "  [" + d + "]" : "")); } };
function throws(f, Ctor, re, w) {
    try { f(); ok(false, w, "no throw"); }
    catch (e) { ok(e instanceof Ctor && (!re || re.test(String(e.message))), w, e.name + ": " + e.message); }
}
function rng(seed) {
    let a = seed >>> 0;
    return () => {
        a = (a + 0x6d2b79f5) >>> 0;
        let t = a;
        t = Math.imul(t ^ (t >>> 15), t | 1);
        t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
        return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    };
}
function data(n, dim, seed, lo = -1, hi = 1) {
    const r = rng(seed), x = new Float32Array(n * dim);
    for (let i = 0; i < x.length; i++) x[i] = lo + r() * (hi - lo);
    return x;
}
function clustered(n, dim, seed) {
    const r = rng(seed), cent = new Float64Array(40 * dim), x = new Float32Array(n * dim);
    for (let i = 0; i < cent.length; i++) cent[i] = r() * 2 - 1;
    for (let i = 0; i < n; i++) {
        const c = Math.floor(r() * 40);
        for (let d = 0; d < dim; d++) x[i * dim + d] = cent[c * dim + d] + 0.15 * Math.sqrt(-2 * Math.log(1 - r())) * Math.cos(2 * Math.PI * r());
    }
    return x;
}
const row = (x, dim, i) => x.subarray(i * dim, (i + 1) * dim);
const METRICS = ["l2", "cosine", "ip"];

function store(metric, x) {
    if (metric !== "cosine") return Float32Array.from(x);
    let s = 0;
    for (let i = 0; i < x.length; i++) s = s + x[i] * x[i];
    const inv = 1 / Math.sqrt(s);
    return Float32Array.from(x, (v) => v * inv);
}
function f32dist(metric, a, b) {
    let s = 0;
    if (metric === "l2") { for (let i = 0; i < a.length; i++) { const t = a[i] - b[i]; s = s + t * t; } return s; }
    for (let i = 0; i < a.length; i++) s = s + a[i] * b[i];
    return metric === "cosine" ? 1 - s : -s;
}

class SQ8 {
    constructor(dim, metric) { this.dim = dim; this.metric = metric; this.trained = false; this.rows = []; }
    refresh() {
        const d = this.dim;
        this.mid = new Float64Array(d); this.msq = 0;
        let half = 0;
        for (let j = 0; j < d; j++) {
            const h = (this.hi[j] - this.lo[j]) * 0.5;
            this.mid[j] = this.lo[j] + h;
            this.msq += this.mid[j] * this.mid[j];
            if (h > half) half = h;
        }
        this.scale = half / 127;
    }
    enc(x) {
        const c = new Int8Array(this.dim);
        let corr = 0;
        for (let j = 0; j < this.dim; j++) {
            const t = this.scale > 0 ? (x[j] - this.mid[j]) / this.scale : 0;
            c[j] = t >= 127 ? 127 : t <= -127 ? -127 : Math.floor(t + 0.5);
            corr += this.mid[j] * c[j];
        }
        return { c, corr: this.metric === "l2" ? 0 : corr };
    }
    addBatch(ids, xs) {
        const d = this.dim, n = ids.length, st = [];
        for (let i = 0; i < n; i++) st.push(store(this.metric, row(xs, d, i)));
        const blo = new Float64Array(d).fill(Infinity), bhi = new Float64Array(d).fill(-Infinity);
        for (const x of st) for (let j = 0; j < d; j++) { if (x[j] < blo[j]) blo[j] = x[j]; if (x[j] > bhi[j]) bhi[j] = x[j]; }
        if (!this.trained) { this.lo = Float64Array.from(blo); this.hi = Float64Array.from(bhi); this.trained = true; this.refresh(); }
        else {
            let grew = false;
            const omid = Float64Array.from(this.mid), os = this.scale;
            for (let j = 0; j < d; j++) {
                let nlo = Math.min(blo[j], this.lo[j]), nhi = Math.max(bhi[j], this.hi[j]);
                const w = nhi - nlo;
                if (blo[j] < this.lo[j]) { nlo -= w * 0.25; grew = true; }
                if (bhi[j] > this.hi[j]) { nhi += w * 0.25; grew = true; }
                this.lo[j] = nlo; this.hi[j] = nhi;
            }
            if (grew) {
                this.refresh();
                for (const r of this.rows) {
                    const x = new Float32Array(d);
                    for (let j = 0; j < d; j++) x[j] = omid[j] + os * r.c[j];
                    Object.assign(r, this.enc(x));
                }
            }
        }
        for (let i = 0; i < n; i++) this.rows.push(Object.assign({ id: Number(ids[i]) }, this.enc(st[i])));
    }
    dist(a, b) {
        const s2 = this.scale * this.scale;
        let t = 0;
        if (this.metric === "l2") { for (let j = 0; j < this.dim; j++) { const x = a.c[j] - b.c[j]; t += x * x; } return s2 * t; }
        for (let j = 0; j < this.dim; j++) t += a.c[j] * b.c[j];
        const dot = this.msq + this.scale * (a.corr + b.corr) + s2 * t;
        return this.metric === "cosine" ? 1 - dot : -dot;
    }
    search(q, k, removed) {
        const qq = this.enc(store(this.metric, q));
        const out = this.rows.filter((r) => !removed || !removed.has(r.id)).map((r) => [this.dist(qq, r), r.id]);
        out.sort((a, b) => a[0] - b[0] || a[1] - b[1]);
        return out.slice(0, k);
    }
}

class BIN {
    constructor(dim, metric) { this.dim = dim; this.metric = metric; this.rows = []; }
    bits(x) { const b = new Uint8Array(this.dim); for (let j = 0; j < this.dim; j++) b[j] = x[j] > 0 ? 1 : 0; return b; }
    addBatch(ids, xs) { for (let i = 0; i < ids.length; i++) this.rows.push({ id: Number(ids[i]), b: this.bits(store(this.metric, row(xs, this.dim, i))) }); }
    search(q, k) {
        const qb = this.bits(store(this.metric, q));
        const out = this.rows.map((r) => { let h = 0; for (let j = 0; j < this.dim; j++) h += qb[j] ^ r.b[j]; return [h, r.id]; });
        out.sort((a, b) => a[0] - b[0] || a[1] - b[1]);
        return out.slice(0, k);
    }
}

const sameHits = (got, want) => got.ids.length === want.length && want.every((w, i) => Number(got.ids[i]) === w[1] && got.distances[i] === Math.fround(w[0]));

await __kitS.section("sq8 kernel equals the documented rule (rerank off, flat)", async () => {
    const dim = 24, n = 1200;
    for (const metric of METRICS) {
        const ix = new VectorIndex({ dims: dim, metric, index: "flat", quantization: "sq8", dyna: { rerank: false } });
        const o = new SQ8(dim, metric);
        const ids = Uint32Array.from({ length: n }, (_, i) => i * 5 + 2), x = data(n, dim, 11);
        await ix.add(ids.subarray(0, 600), x.subarray(0, 600 * dim)); o.addBatch(ids.subarray(0, 600), x.subarray(0, 600 * dim));
        const q = data(40, dim, 12, -1.3, 1.3);
        let eq = 0;
        for (let i = 0; i < 40; i++) if (sameHits(await ix.search(row(q, dim, i), 15), o.search(row(q, dim, i), 15))) eq++;
        ok(eq === 40, metric + ": first batch: ids and distances equal the oracle (out-of-range queries clamp)", eq + "/40");
        const wide = data(600, dim, 13, -1.6, 2.4);
        await ix.add(ids.subarray(600), wide); o.addBatch(ids.subarray(600), wide);
        eq = 0;
        for (let i = 0; i < 40; i++) if (sameHits(await ix.search(row(q, dim, i), 15), o.search(row(q, dim, i), 15))) eq++;
        ok(eq === 40, metric + ": after a batch that widens the range and re-encodes from codes", eq + "/40");
        const removed = new Set();
        ix.remove(ids.filter((_, i) => i % 3 === 0));
        for (let i = 0; i < n; i += 3) removed.add(ids[i]);
        eq = 0;
        for (let i = 0; i < 40; i++) if (sameHits(await ix.search(row(q, dim, i), 15), o.search(row(q, dim, i), 15, removed))) eq++;
        ok(eq === 40, metric + ": tombstones are skipped by the quantized scan", eq + "/40");
        const one = new VectorIndex({ dims: 3, metric, index: "flat", quantization: "sq8", dyna: { rerank: false } });
        await one.add(Uint32Array.of(1, 2), Float32Array.of(0.5, 0.5, 0.5, 0.5, 0.5, 0.5));
        const r = await one.search(Float32Array.of(0.5, 0.5, 0.5), 2);
        ok(r.ids.length === 2 && r.ids[0] === 1n && r.distances[0] === r.distances[1], metric + ": a zero-width range codes everything as 0 (ties by id)", Array.from(r.distances).join());
    }
});

await __kitS.section("binary kernel equals the documented rule (rerank off, flat)", async () => {
    const dim = 70, n = 900;
    for (const metric of METRICS) {
        const ix = new VectorIndex({ dims: dim, metric, index: "flat", quantization: "binary", dyna: { rerank: false } });
        const o = new BIN(dim, metric), ids = Uint32Array.from({ length: n }, (_, i) => n - i), x = data(n, dim, 21);
        await ix.add(ids, x); o.addBatch(ids, x);
        const q = data(40, dim, 22);
        let eq = 0;
        for (let i = 0; i < 40; i++) if (sameHits(await ix.search(row(q, dim, i), 12), o.search(row(q, dim, i), 12))) eq++;
        ok(eq === 40, metric + ": Hamming ids and bit-count distances equal the oracle (70 dims: a partial last word)", eq + "/40");
        const z = new VectorIndex({ dims: 2, metric: "l2", index: "flat", quantization: "binary", dyna: { rerank: false } });
        await z.add(Uint32Array.of(1, 2, 3), Float32Array.of(0, 0, 1, -1, -1, 1));
        const r = await z.search(Float32Array.of(1, 1), 3);
        ok(Array.from(r.ids).join() === "2,3,1" && Array.from(r.distances).join() === "1,1,2", "0 is not > 0: the zero vector has no bits", Array.from(r.ids).join() + " " + Array.from(r.distances).join());
    }
});

await __kitS.section("rerank on returns f32 distances", async () => {
    const dim = 20, n = 1500, x = clustered(n, dim, 31), ids = Uint32Array.from({ length: n }, (_, i) => i), q = clustered(30, dim, 32);
    for (const metric of METRICS) {
        const f = new VectorIndex({ dims: dim, metric, index: "flat" });
        await f.add(ids, x);
        for (const quantization of ["sq8", "binary"]) {
            const all = new VectorIndex({ dims: dim, metric, index: "flat", quantization, dyna: { efSearch: n } });
            const h = new VectorIndex({ dims: dim, metric, quantization });
            const fl = new VectorIndex({ dims: dim, metric, index: "flat", quantization });
            for (const ix of [all, h, fl]) await ix.add(ids, x);
            let exact = 0, hOk = 0, flOk = 0;
            for (let i = 0; i < 30; i++) {
                const qi = row(q, dim, i), want = await f.search(qi, 10), got = await all.search(qi, 10);
                if (Array.from(got.ids).join() === Array.from(want.ids).join() && Array.from(got.distances).join() === Array.from(want.distances).join()) exact++;
                const sq = store(metric, qi);
                for (const [ix, inc] of [[h, () => hOk++], [fl, () => flOk++]]) {
                    const r = await ix.search(qi, 10);
                    let good = r.ids.length === 10;
                    for (let t = 0; t < r.ids.length && good; t++) {
                        good = r.distances[t] === Math.fround(f32dist(metric, sq, store(metric, row(x, dim, Number(r.ids[t])))));
                        if (t) good = good && (r.distances[t - 1] < r.distances[t] || (r.distances[t - 1] === r.distances[t] && r.ids[t - 1] < r.ids[t]));
                    }
                    if (good) inc();
                }
            }
            ok(exact === 30, metric + " " + quantization + ": flat reranking every vector equals the f32 index", exact + "/30");
            ok(hOk === 30 && flOk === 30, metric + " " + quantization + ": every returned distance is the f32 distance, nearest first", hOk + " " + flOk);
            for (const ix of [all, h, fl]) ix.close();
        }
        f.close();
    }
});

await __kitS.section("candidate pool: max(efSearch, k) x rerankFactor, clamped to the size", async () => {
    const dim = 70, n = 900, ids = Uint32Array.from({ length: n }, (_, i) => n - i), x = data(n, dim, 81), q = data(20, dim, 82);
    const dir = makeTempDir("vecpool-");
    for (const metric of METRICS) {
        const xs = new Map();
        for (let i = 0; i < n; i++) xs.set(ids[i], store(metric, row(x, dim, i)));
        for (const quantization of ["binary", "sq8"]) {
            const o = quantization === "binary" ? new BIN(dim, metric) : new SQ8(dim, metric);
            o.addBatch(ids, x);
            const pooled = (qi, k, P) => o.search(qi, P).map(([, id]) => [f32dist(metric, store(metric, qi), xs.get(id)), id]).sort((a, b) => a[0] - b[0] || a[1] - b[1]).slice(0, k);
            const def = quantization === "binary" ? 12 : 1;
            for (const [factor, ef, k] of [[0, 8, 5], [3, 8, 5], [1, 8, 5], [2, 3, 7], [5, 40, 10], [64, 128, 10]]) {
                const ix = new VectorIndex({ dims: dim, metric, index: "flat", quantization, dyna: factor ? { rerankFactor: factor } : {} });
                await ix.add(ids, x);
                const P = Math.min(Math.max(ef, k) * (factor || def), n), t = `${metric} ${quantization} rerankFactor ${factor || "default " + def} efSearch ${ef} k ${k}: `;
                let eq = 0;
                for (let i = 0; i < 20; i++) if (sameHits(await ix.search(row(q, dim, i), k, { efSearch: ef }), pooled(row(q, dim, i), k, P))) eq++;
                ok(eq === 20, t + "the best " + P + " candidates by the quantized distance are reranked with f32", eq + "/20");
                if (factor === 3) {
                    const f = dir.join(metric + quantization + ".vec");
                    ix.save(f, { fsync: false });
                    for (const mmap of [false, true]) {
                        const l = VectorIndex.load(f, { mmap });
                        let le = 0;
                        for (let i = 0; i < 20; i++) if (sameHits(await l.search(row(q, dim, i), k, { efSearch: ef }), pooled(row(q, dim, i), k, P))) le++;
                        ok(le === 20, t + (mmap ? "mmap" : "copy") + ": a loaded index keeps the factor", le + "/20");
                        l.close();
                    }
                }
                ix.close();
            }
        }
    }
    removeAll(dir);
});

await __kitS.section("max dims with extreme codes", async () => {
    const D = 65536, a = new Float32Array(D).fill(1), b = new Float32Array(D).fill(-1);
    const ab = new Float32Array(2 * D); ab.set(a); ab.set(b, D);
    const l2 = new VectorIndex({ dims: D, index: "flat", quantization: "sq8", dyna: { rerank: false } });
    await l2.add(Uint32Array.of(1, 2), ab);
    let r = await l2.search(a, 2);
    ok(Array.from(r.ids).join() === "1,2" && r.distances[0] === 0 && r.distances[1] === 262144, "l2: 65536 dims of code difference 254 sum past int32 (262144 = 65536 * 2^2)", Array.from(r.distances).join());
    const ip = new VectorIndex({ dims: D, metric: "ip", index: "flat", quantization: "sq8", dyna: { rerank: false } });
    await ip.add(Uint32Array.of(1, 2), ab);
    r = await ip.search(a, 2);
    ok(Array.from(r.ids).join() === "1,2" && r.distances[0] === -65536 && r.distances[1] === 65536, "ip: the code dot product at 65536 dims", Array.from(r.distances).join());
    const bin = new VectorIndex({ dims: D, index: "flat", quantization: "binary", dyna: { rerank: false } });
    await bin.add(Uint32Array.of(1, 2), ab);
    r = await bin.search(a, 2);
    ok(r.distances[0] === 0 && r.distances[1] === 65536, "binary: 1024 words of popcount", Array.from(r.distances).join());
    for (const ix of [l2, ip, bin]) ix.close();
});

const recallRows = [];
await __kitS.section("recall@10 against f32 flat", async () => {
    const dim = 64, n = 4000, nq = 40;
    for (const [set, gen] of [["clustered", clustered], ["uniform", data]]) {
    const x = gen(n, dim, 41), q = gen(nq, dim, 42), ids = Uint32Array.from({ length: n }, (_, i) => i);
    for (const metric of METRICS) {
        const f = new VectorIndex({ dims: dim, metric, index: "flat" });
        await f.add(ids, x);
        const truth = [];
        for (let i = 0; i < nq; i++) truth.push(new Set(Array.from((await f.search(row(q, dim, i), 10)).ids)));
        for (const quantization of ["sq8", "binary"]) for (const index of ["flat", "hnsw"]) for (const rerank of [true, false]) {
            const ix = new VectorIndex({ dims: dim, metric, index, quantization, dyna: { rerank } });
            await ix.add(ids.subarray(0, 2000), x.subarray(0, 2000 * dim));
            await ix.add(ids.subarray(2000), x.subarray(2000 * dim));
            let hit = 0;
            for (let i = 0; i < nq; i++) for (const id of (await ix.search(row(q, dim, i), 10)).ids) if (truth[i].has(id)) hit++;
            const rc = hit / (nq * 10);
            recallRows.push(`${set.padEnd(9)} ${metric.padEnd(6)} ${quantization.padEnd(6)} ${index.padEnd(4)} rerank ${String(rerank).padEnd(5)} recall@10 ${rc.toFixed(4)}`);
            const floor = quantization === "sq8" ? (rerank ? 0.95 : 0.9) : rerank ? 0.9 : 0;
            ok(rc >= floor, `${set} ${metric} ${quantization} ${index} rerank ${rerank}: recall@10 >= ${floor}`, rc.toFixed(4));
            ix.close();
            if (quantization === "binary" && rerank) {
                const one = new VectorIndex({ dims: dim, metric, index, quantization, dyna: { rerankFactor: 1 } });
                await one.add(ids, x);
                let h1 = 0;
                for (let i = 0; i < nq; i++) for (const id of (await one.search(row(q, dim, i), 10)).ids) if (truth[i].has(id)) h1++;
                recallRows.push(`${set.padEnd(9)} ${metric.padEnd(6)} binary ${index.padEnd(4)} rerank true  rerankFactor 1 recall@10 ${(h1 / (nq * 10)).toFixed(4)}`);
                ok(h1 < rc * nq * 10, `${set} ${metric} binary ${index}: the default factor beats reranking only the beam`, (h1 / (nq * 10)).toFixed(4));
                one.close();
            }
        }
        f.close();
    }
    }
    for (const r of recallRows) print("  " + r);
});

await __kitS.section("filters and tombstones on every quantized path", async () => {
    const dim = 16, n = 3000, x = data(n, dim, 51), q = data(20, dim, 52), r = rng(53);
    const ids = BigUint64Array.from({ length: n }, (_, i) => (i % 2 ? 2n ** 40n + BigInt(i) : BigInt(i)));
    const gone = new Set(), rb = new RoaringBitmap(), sel = new Set();
    for (let i = 0; i < n; i += 4) gone.add(ids[i]);
    for (let i = 0; i < n; i++) if (r() < 0.04) sel.add(ids[i]);
    const small = BigUint64Array.from([...sel].sort((a, b) => (a < b ? -1 : 1)));
    const half = BigUint64Array.from(ids.filter((_, i) => i % 3 !== 2)).sort();
    for (const v of ids) if (v < 2n ** 32n && v % 5n === 0n) rb.add(Number(v));
    for (const quantization of ["sq8", "binary"]) for (const rerank of [true, false]) {
        const make = (o) => new VectorIndex(Object.assign({ dims: dim, quantization }, o, { dyna: Object.assign({ rerank }, o.dyna || {}) }));
        const flat = make({ index: "flat" }), h = make({}), beam = make({ dyna: { exactFallback: false } });
        for (const ix of [flat, h, beam]) { await ix.add(ids, x); ix.remove(BigUint64Array.from(gone)); }
        const t = quantization + " rerank " + rerank + ": ";
        let good = 0, total = 0, sizes = 0;
        for (const [ix, f, allow] of [[flat, small, (v) => sel.has(v)], [h, small, (v) => sel.has(v)], [beam, half, (v) => half.includes(v)], [h, half, (v) => half.includes(v)], [h, rb, (v) => v < 2n ** 32n && v % 5n === 0n], [flat, rb, (v) => v < 2n ** 32n && v % 5n === 0n]]) {
            for (let i = 0; i < 20; i++) {
                const res = await ix.search(row(q, dim, i), 10, { filter: f });
                total++;
                sizes += res.ids.length;
                if (Array.from(res.ids).every((v) => allow(v) && !gone.has(v))) good++;
            }
        }
        ok(good === total, t + "every hit passes the filter and is live (flat, fallback, beam; 64-bit and RoaringBitmap)", good + "/" + total);
        ok(sizes > total * 5, t + "the filtered searches return results", String(sizes));
        const b = await h.searchBatch(q.subarray(0, 3 * dim), 4, { filter: small });
        ok(b.ids.length === 12 && Array.from(b.counts).every((c) => c === 4) && Array.from(b.ids).every((v) => sel.has(v) && !gone.has(v)), t + "searchBatch with a 64-bit filter");
        for (const ix of [flat, h, beam]) ix.close();
    }
});

const u32 = (b, o) => (b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24)) >>> 0;
const u64 = (b, o) => BigInt(u32(b, o)) + (BigInt(u32(b, o + 4)) << 32n);
function layout(b) {
    const h = 64, ns = u32(b, h + 24), secs = {};
    for (let i = 0; i < ns; i++) {
        const e = h + 160 + 40 * i;
        secs[u32(b, e)] = { entry: e, kind: u32(b, e), off: Number(u64(b, e + 16)), len: Number(u64(b, e + 24)) };
    }
    return { ns, secs };
}
function reseal(b) {
    const L = layout(b), dv = new DataView(b.buffer, b.byteOffset, b.length);
    for (const s of Object.values(L.secs)) dv.setUint32(s.entry + 32, CRC32C(b.subarray(s.off, s.off + s.len)) >>> 0, true);
    dv.setUint32(64 + 128, 0, true);
    dv.setUint32(64 + 128, CRC32C(b.subarray(64, 64 + 160 + 40 * L.ns)) >>> 0, true);
    return b;
}
const forge = (img, edit) => { const b = Uint8Array.from(img), L = layout(b); edit(b, L, new DataView(b.buffer)); return reseal(b); };
const META = 48, DATA = 49, QPARAM = 54, CODES = 55;
const metaSet = (dv, L, i, v) => dv.setBigUint64(L.secs[META].off + 8 * i, BigInt(v), true);
const metaGet = (dv, L, i) => dv.getBigUint64(L.secs[META].off + 8 * i, true);
const bytesEq = (a, b) => a.length === b.length && a.every((v, i) => v === b[i]);
const codeOf = (fn) => { try { const v = fn(); v.close(); return "loaded"; } catch (e) { return e && e.code ? e.code : "uncoded:" + e; } };

await __kitS.section("save/load: byte identity, mmap, lockstep through a widening add", async () => {
    const dir = makeTempDir("vecq-");
    const dim = 10, n = 700, x = data(n, dim, 61), q = data(8, dim, 62);
    const ids = BigUint64Array.from({ length: n }, (_, i) => BigInt(i) * 3n + (i % 2 ? 2n ** 33n : 0n));
    let no = 0;
    const p = () => dir.join("f" + (no++) + ".vec");
    const answers = async (ix) => { const o = []; for (let i = 0; i < 8; i++) { const r = await ix.search(row(q, dim, i), 6); o.push(Array.from(r.ids).join() + "|" + Array.from(r.distances).join()); } return o.join(";"); };
    for (const metric of METRICS) for (const quantization of ["sq8", "binary"]) for (const index of ["flat", "hnsw"]) for (const rerank of [true, false]) {
        const t = `${metric} ${quantization} ${index} rerank ${rerank}: `;
        const ix = new VectorIndex({ dims: dim, metric, index, quantization, dyna: { rerank } });
        await ix.add(ids.subarray(0, 400), x.subarray(0, 400 * dim));
        ix.remove(ids.subarray(0, 40));
        const f1 = p();
        ix.save(f1, { fsync: false });
        const img = readBytes(f1), L = layout(img);
        let ref = null, refAns = null;
        ok(!!L.secs[CODES] && !!L.secs[QPARAM] === (quantization === "sq8") && (L.secs[DATA].len > 0) === rerank, t + "sections follow the quantization and rerank");
        const want = await answers(ix), savedSize = ix.size;
        for (const mmap of [false, true]) {
            const l = VectorIndex.load(f1, { mmap });
            ok(l.quantization === quantization && l.size === savedSize && (await answers(l)) === want, t + (mmap ? "mmap" : "copy") + ": loaded answers identically");
            const f2 = p();
            l.save(f2, { fsync: false });
            ok(bytesEq(readBytes(f2), img), t + (mmap ? "mmap" : "copy") + ": save after load reproduces the file");
            const more = x.subarray(400 * dim).map((v) => v * 3);
            await l.add(ids.subarray(400), more);
            if (!mmap) await ix.add(ids.subarray(400), more);
            const fa = p();
            l.save(fa, { fsync: false });
            if (!mmap) { ref = p(); ix.save(ref, { fsync: false }); refAns = await answers(ix); }
            ok(bytesEq(readBytes(fa), readBytes(ref)) && (await answers(l)) === refAns, t + (mmap ? "mmap" : "copy") + ": loaded and original stay in lockstep through an add 3x outside the range");
            ok(bytesEq(readBytes(f1), img), t + (mmap ? "mmap" : "copy") + ": the loaded file is never written");
            l.close();
        }
        ix.close();
    }
    removeAll(dir);
});

await __kitS.section("forged quantization records are refused", async () => {
    const dir = makeTempDir("vecqf-");
    const mk = async (quantization, rerank, dim = 70) => {
        const o = quantization === "f32" ? { dims: dim } : { dims: dim, quantization, dyna: { rerank } };
        const ix = new VectorIndex(o), f = dir.join(quantization + rerank + ".vec");
        await ix.add(Uint32Array.from({ length: 50 }, (_, i) => i), data(50, dim, 71));
        ix.save(f, { fsync: false }); ix.close();
        return readBytes(f);
    };
    const bad = dir.join("bad.vec");
    const refuse = (img, what, edit) => {
        writeFile(bad, forge(img, edit));
        for (const mmap of [false, true]) { const c = codeOf(() => VectorIndex.load(bad, { mmap })); ok(c === "EPERSIST_CORRUPT", "forged: " + what + (mmap ? " (mmap)" : ""), c); }
    };
    const sq = await mk("sq8", true), sqc = await mk("sq8", false), bn = await mk("binary", true), bnc = await mk("binary", false), f32 = await mk("f32", true);
    writeFile(bad, sq);
    ok(codeOf(() => VectorIndex.load(bad)) === "loaded", "the untouched sq8 file loads");
    refuse(sq, "quantization 3", (b, L, dv) => metaSet(dv, L, 8, 3));
    refuse(sq, "sq8 file claiming f32", (b, L, dv) => { metaSet(dv, L, 8, 0); metaSet(dv, L, 7, metaGet(dv, L, 7) & 3n); });
    refuse(f32, "f32 file claiming sq8 (no codes)", (b, L, dv) => metaSet(dv, L, 8, 1));
    refuse(sq, "sq8 file claiming binary (code size)", (b, L, dv) => { metaSet(dv, L, 8, 2); metaSet(dv, L, 7, metaGet(dv, L, 7) & 7n); });
    refuse(bn, "binary file claiming sq8 (no range)", (b, L, dv) => metaSet(dv, L, 8, 1));
    refuse(sq, "flags 16", (b, L, dv) => metaSet(dv, L, 7, 16));
    refuse(sq, "f32 copy dropped from the flags only", (b, L, dv) => metaSet(dv, L, 7, metaGet(dv, L, 7) | 4n));
    refuse(sqc, "codes-only file claiming an f32 copy", (b, L, dv) => metaSet(dv, L, 7, metaGet(dv, L, 7) & ~4n));
    refuse(sq, "trained flag cleared with vectors present", (b, L, dv) => metaSet(dv, L, 7, metaGet(dv, L, 7) & ~8n));
    refuse(bn, "binary with the trained flag", (b, L, dv) => metaSet(dv, L, 7, metaGet(dv, L, 7) | 8n));
    refuse(f32, "f32 with the codes-only flag", (b, L, dv) => metaSet(dv, L, 7, metaGet(dv, L, 7) | 4n));
    refuse(sq, "sq8 lo NaN", (b, L, dv) => dv.setFloat64(L.secs[QPARAM].off, NaN, true));
    refuse(sq, "sq8 hi Infinity", (b, L, dv) => dv.setFloat64(L.secs[QPARAM].off + 8 * 70, Infinity, true));
    refuse(sq, "sq8 lo above hi", (b, L, dv) => dv.setFloat64(L.secs[QPARAM].off + 8 * 3, 50, true));
    refuse(sq, "sq8 range section short", (b, L, dv) => dv.setBigUint64(L.secs[QPARAM].entry + 24, BigInt(L.secs[QPARAM].len - 8), true));
    refuse(sqc, "sq8 code -128", (b, L) => { b[L.secs[CODES].off + 17] = 0x80; });
    refuse(sq, "sq8 code section short", (b, L, dv) => dv.setBigUint64(L.secs[CODES].entry + 24, BigInt(L.secs[CODES].len - 1), true));
    refuse(bn, "binary bit past dims", (b, L) => { b[L.secs[CODES].off + 8 + 7] |= 0x80; });
    refuse(bnc, "binary bit past dims (codes only)", (b, L) => { b[L.secs[CODES].off + 16 * 3 + 15] |= 0x40; });
    refuse(bn, "binary code section with the sq8 element size", (b, L, dv) => dv.setUint32(L.secs[CODES].entry + 8, 1, true));
    refuse(f32, "f32 data dropped while claiming f32", (b, L, dv) => dv.setBigUint64(L.secs[DATA].entry + 24, 0n, true));
    refuse(bn, "binary rerank factor 0", (b, L, dv) => metaSet(dv, L, 15, 0));
    refuse(bn, "binary rerank factor 65", (b, L, dv) => metaSet(dv, L, 15, 65));
    refuse(sq, "sq8 rerank factor 0", (b, L, dv) => metaSet(dv, L, 15, 0));
    refuse(bnc, "codes-only binary with a rerank factor", (b, L, dv) => metaSet(dv, L, 15, 1));
    refuse(sqc, "codes-only sq8 with a rerank factor", (b, L, dv) => metaSet(dv, L, 15, 12));
    refuse(f32, "f32 with a rerank factor", (b, L, dv) => metaSet(dv, L, 15, 1));
    writeFile(bad, forge(bn, (b, L, dv) => { ok(metaGet(dv, L, 15) === 12n && metaGet(dv, L, 7) < 4n, "a binary file records the default factor 12", String(metaGet(dv, L, 15))); metaSet(dv, L, 15, 64); }));
    ok(codeOf(() => VectorIndex.load(bad)) === "loaded", "factor 64 (the maximum) loads");
    writeFile(bad, forge(sq, (b, L, dv) => ok(metaGet(dv, L, 15) === 1n, "an sq8 file records the default factor 1", String(metaGet(dv, L, 15)))));
    writeFile(bad, forge(f32, (b, L, dv) => ok(metaGet(dv, L, 15) === 0n, "an f32 file keeps word 15 zero", String(metaGet(dv, L, 15)))));
    refuse(f32, "f32 claiming codes only with its data dropped", (b, L, dv) => { metaSet(dv, L, 7, metaGet(dv, L, 7) | 4n); dv.setBigUint64(L.secs[DATA].entry + 24, 0n, true); });
    removeAll(dir);
});

await __kitS.section("options", async () => {
    throws(() => new VectorIndex({ dims: 4, dyna: { rerank: false } }), TypeError, /rerank applies only to quantization/, "rerank with f32");
    throws(() => new VectorIndex({ dims: 4, quantization: "sq8", dyna: { rerank: 1 } }), TypeError, /rerank must be a boolean/, "rerank not boolean");
    throws(() => new VectorIndex({ dims: 4, index: "flat", quantization: "sq8", dyna: { seed: 1 } }), TypeError, /only to index "hnsw"/, "seed with quantized flat");
    throws(() => new VectorIndex({ dims: 4, index: "flat", dyna: { efSearch: 8 } }), TypeError, /only to index "hnsw"/, "efSearch with f32 flat");
    throws(() => new VectorIndex({ dims: 4, index: "ivfpq", quantization: "sq8" }), RangeError, /"ivfpq" is unsupported in this build/, "ivfpq stays unsupported");
    throws(() => new VectorIndex({ dims: 4, dyna: { rerankFactor: 2 } }), TypeError, /rerankFactor applies only to quantization "sq8" or "binary" with dyna.rerank true/, "rerankFactor with f32");
    throws(() => new VectorIndex({ dims: 4, quantization: "binary", dyna: { rerank: false, rerankFactor: 2 } }), TypeError, /rerankFactor applies only/, "rerankFactor with rerank false");
    throws(() => new VectorIndex({ dims: 4, quantization: "binary", dyna: { rerankFactor: "2" } }), TypeError, /rerankFactor must be a number/, "rerankFactor not a number");
    for (const bad of [0, 65, 1.5, NaN, Infinity]) throws(() => new VectorIndex({ dims: 4, quantization: "sq8", dyna: { rerankFactor: bad } }), RangeError, /rerankFactor must be an integer in \[1, 64\]/, "rerankFactor " + bad);
    ok(new VectorIndex({ dims: 4, index: "flat", quantization: "sq8", dyna: { rerankFactor: 64 } }).index === "flat", "a quantized flat index takes rerankFactor");
    const fl = new VectorIndex({ dims: 4, index: "flat", quantization: "binary", dyna: { efSearch: 8, rerank: false } });
    ok(fl.quantization === "binary" && fl.index === "flat", "a quantized flat index takes efSearch and rerank");
    ok(new VectorIndex({ dims: 4 }).quantization === "f32", "default quantization getter");
    const e = new VectorIndex({ dims: 4, quantization: "sq8" });
    const r = await e.search(Float32Array.of(1, 2, 3, 4), 3);
    ok(r.ids.length === 0, "an empty untrained sq8 index returns nothing");
    await e.add(Uint32Array.of(9), Float32Array.of(1, 2, 3, 4));
    ok(Number((await e.search(Float32Array.of(0, 0, 0, 0), 1)).ids[0]) === 9, "one vector trains the range");
});

__kitS.end();
print(`test_vec_vector_index_quantization: ${pass} passed, ${fail} failed`);
if (fail) throw new Error("test_vec_vector_index_quantization: " + fail + " failing checks");
