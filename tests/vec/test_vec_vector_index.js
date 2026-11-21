// timeout: 300
// covers: dyna:vec.VectorIndex
// dyna:vec VectorIndex (flat + HNSW), in the owner's shape: dims/m/efConstruction, add(ids, vectors) and search()
// returning Promises, 64-bit ids, remove() with tombstones, DynResource, dyna-specific meta options in `dyna`.
// Oracles: a brute-force scan in JS that stores vectors the way the contract says (cosine scaled to unit length,
// rounded to f32) and computes distances in double over the f32 components; Flat must equal it (ids exactly,
// distances equal to the f32 rounding of the oracle within one f32 step). HNSW is measured as recall@10 against
// Flat on seeded uniform and clustered data for each metric at default parameters (floor 0.95, numbers printed).
// Filtered search is checked against brute force over the filtered set, with the exact-fallback boundary
// (card * 16 <= size) pinned on both sides. remove() is checked against a model: removed ids never come back,
// with or without filters or the fallback, re-adds get fresh vectors, and the u64 id map (open addressing with
// backward-shift deletion) agrees with a JS Map through random add/remove churn. Also: ties by id, k > size,
// empty index, determinism per seed, call-order queueing, hostile option getters, DynResource, every error.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_vec_vector_index");
import { VectorIndex } from "dyna:vec";
import { RoaringBitmap, BitSet } from "dyna:structures";

let pass = 0, fail = 0;
const ok = (c, w, d) => { if (c) pass++; else { fail++; print("  FAIL  " + w + (d ? "  [" + d + "]" : "")); } };
function throws(f, Ctor, re, w) {
    try { f(); ok(false, w, "no throw"); }
    catch (e) { ok(e instanceof Ctor && (!re || re.test(String(e.message))), w, e.name + ": " + e.message); }
}
async function rejects(p, Ctor, re, w) {
    let r;
    try { r = typeof p === "function" ? p() : p; } catch (e) { ok(false, w, "threw synchronously: " + e.message); return; }
    ok(r instanceof Promise, w + " returns a Promise");
    try { await r; ok(false, w, "no rejection"); }
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

function gauss(r) {
    const u = 1 - r(), v = r();
    return Math.sqrt(-2 * Math.log(u)) * Math.cos(2 * Math.PI * v);
}

function uniformData(n, dim, seed) {
    const r = rng(seed), x = new Float32Array(n * dim);
    for (let i = 0; i < x.length; i++) x[i] = r() * 2 - 1;
    return x;
}

function clusteredData(n, dim, seed, nc = 40, sigma = 0.08) {
    const r = rng(seed), cent = new Float64Array(nc * dim), x = new Float32Array(n * dim);
    for (let i = 0; i < cent.length; i++) cent[i] = r() * 2 - 1;
    for (let i = 0; i < n; i++) {
        const c = Math.floor(r() * nc);
        const scale = 0.5 + r();
        for (let d = 0; d < dim; d++) x[i * dim + d] = (cent[c * dim + d] + sigma * gauss(r)) * scale;
    }
    return x;
}

function store(metric, x) {
    if (metric !== "cosine") return Float32Array.from(x);
    let s = 0;
    for (let i = 0; i < x.length; i++) s = s + x[i] * x[i];
    const inv = 1 / Math.sqrt(s);
    return Float32Array.from(x, (v) => v * inv);
}

function dist(metric, a, b) {
    let s = 0;
    if (metric === "l2") {
        for (let i = 0; i < a.length; i++) { const t = a[i] - b[i]; s = s + t * t; }
        return s;
    }
    for (let i = 0; i < a.length; i++) s = s + a[i] * b[i];
    return metric === "cosine" ? 1 - s : -s;
}

class Brute {
    constructor(dim, metric) { this.dim = dim; this.metric = metric; this.rows = new Map(); }
    add(id, x) { this.rows.set(id, store(this.metric, x)); }
    remove(id) { this.rows.delete(id); }
    addAll(ids, xs) { for (let i = 0; i < ids.length; i++) this.add(Number(ids[i]), xs.subarray(i * this.dim, (i + 1) * this.dim)); }
    search(q, k, allow) {
        const qs = store(this.metric, q), out = [];
        for (const [id, x] of this.rows) if (!allow || allow(id)) out.push([dist(this.metric, qs, x), id]);
        out.sort((a, b) => a[0] - b[0] || a[1] - b[1]);
        const top = out.slice(0, k);
        return { ids: top.map((t) => t[1]), distances: top.map((t) => t[0]) };
    }
}

const idsOf = (r) => Array.from(r.ids, Number);
const sameIds = (a, b) => a.length === b.length && Array.from(a).every((x, i) => Number(x) === Number(b[i]));
const closeF = (a, b) => a.length === b.length && Array.from(a).every((x, i) => Math.abs(x - Math.fround(b[i])) <= 1.2e-7 * Math.max(1, Math.abs(b[i])));
const row = (x, dim, i) => x.subarray(i * dim, (i + 1) * dim);
const seqIds = (n, mul = 1, add = 0) => Uint32Array.from({ length: n }, (_, i) => i * mul + add);
const one = (id) => (typeof id === "bigint" ? BigUint64Array.of(id) : Uint32Array.of(id));
const METRICS = ["l2", "cosine", "ip"];

await __kitS.section("Flat equals brute force", async () => {
    const dims = 16, n = 1500;
    for (const metric of METRICS) {
        const x = uniformData(n, dims, 11), ids = seqIds(n, 7, 3), q = uniformData(60, dims, 12);
        const f = new VectorIndex({ dims, metric, index: "flat" }), b = new Brute(dims, metric);
        ok(await f.add(ids, x) === undefined && f.size === n, metric + ": add resolves to undefined and grows size");
        b.addAll(ids, x);
        let idsOk = 0, dOk = 0;
        for (let i = 0; i < 60; i++) {
            for (const k of [1, 10, 37]) {
                const got = await f.search(row(q, dims, i), k), want = b.search(row(q, dims, i), k);
                if (sameIds(got.ids, want.ids)) idsOk++;
                if (closeF(got.distances, want.distances)) dOk++;
            }
        }
        ok(idsOk === 180, metric + ": flat ids equal brute force on every query", idsOk + "/180");
        ok(dOk === 180, metric + ": flat distances equal the f32 rounding of brute force", dOk + "/180");
        const r = await f.search(row(q, dims, 0), 3);
        ok(r.ids instanceof BigUint64Array && r.distances instanceof Float32Array, metric + ": result types");
        ok(r.distances[0] <= r.distances[1] && r.distances[1] <= r.distances[2], metric + ": nearest first");
        f.close();
    }
});

await __kitS.section("hand-checked distances and ties", async () => {
    const l2 = new VectorIndex({ dims: 2, metric: "l2", index: "flat" });
    await l2.add(Uint32Array.of(5, 9), Float32Array.of(3, 4, 0, 0));
    let r = await l2.search(Float32Array.of(0, 0), 2);
    ok(sameIds(r.ids, [9, 5]) && r.distances[0] === 0 && r.distances[1] === 25, "l2 is the squared distance", Array.from(r.distances).join());
    const ip = new VectorIndex({ dims: 2, metric: "ip", index: "flat" });
    await ip.add(Uint32Array.of(1, 2), Float32Array.of(1, 0, 3, 1));
    r = await ip.search(Float32Array.of(2, 1), 2);
    ok(sameIds(r.ids, [2, 1]) && r.distances[0] === -7 && r.distances[1] === -2, "ip is -dot (larger dot first)", Array.from(r.distances).join());
    const cs = new VectorIndex({ dims: 2, metric: "cosine", index: "flat" });
    await cs.add(Uint32Array.of(1, 2, 3), Float32Array.of(10, 0, 0, 0.5, -4, 0));
    r = await cs.search(Float32Array.of(7, 0), 3);
    ok(sameIds(r.ids, [1, 2, 3]) && r.distances[0] === 0 && r.distances[1] === 1 && r.distances[2] === 2, "cosine is 1 - cos, scale-free", Array.from(r.distances).join());
    for (const index of ["flat", "hnsw"]) {
        const t = new VectorIndex({ dims: 3, index });
        const order = [40, 7, 99, 3, 12, 0, 65, 21];
        await t.add(Uint32Array.from(order), new Float32Array(order.length * 3).map((_, i) => (i % 3) + 1));
        await t.add(Uint32Array.of(1000), Float32Array.of(1, 2, 4));
        r = await t.search(Float32Array.of(1, 2, 3), 9);
        ok(sameIds(r.ids, [0, 3, 7, 12, 21, 40, 65, 99, 1000]), index + ": equal distances rank by id", idsOf(r).join());
        r = await t.search(Float32Array.of(1, 2, 3), 4);
        ok(sameIds(r.ids, [0, 3, 7, 12]), index + ": k cuts the tie run by id", idsOf(r).join());
    }
});

await __kitS.section("k > size, empty index, getters", async () => {
    for (const index of ["flat", "hnsw"]) {
        const e = new VectorIndex({ dims: 4, index });
        ok(e.dims === 4 && e.metric === "l2" && e.index === index && e.size === 0 && e.closed === false, index + ": getters");
        let r = await e.search(Float32Array.of(1, 2, 3, 4), 10);
        ok(r.ids.length === 0 && r.distances.length === 0, index + ": empty index returns empty arrays");
        const b = await e.searchBatch(Float32Array.of(1, 2, 3, 4, 5, 6, 7, 8), 3);
        ok(b.ids.length === 6 && b.ids.every((x) => x === 18446744073709551615n) && b.distances.every((x) => x === Infinity) && sameIds(b.counts, [0, 0]), index + ": empty searchBatch pads every slot");
        await e.add(Uint32Array.of(1, 2), Float32Array.of(0, 0, 0, 1, 0, 0, 1, 0));
        r = await e.search(Float32Array.of(0, 0, 0, 1), 65536);
        ok(sameIds(r.ids, [1, 2]), index + ": k above size returns every vector", idsOf(r).join());
        ok(e.size === 2 && e.has(1) && e.has(2n) && !e.has(3) && !e.has(-1) && !e.has(1.5) && !e.has(2 ** 64) && !e.has(-1n) && !e.has(2n ** 64n), index + ": size and has");
    }
});

const recallLog = [];
await __kitS.section("HNSW recall@10 vs Flat at default parameters", async () => {
    const dims = 32, n = 4000, nq = 200, k = 10;
    for (const [kind, gen] of [["uniform", uniformData], ["clustered", clusteredData]]) {
        for (const metric of METRICS) {
            const x = gen(n, dims, 101), q = gen(nq, dims, 202), ids = seqIds(n, 3, 1);
            const h = new VectorIndex({ dims, metric }), f = new VectorIndex({ dims, metric, index: "flat" });
            await h.add(ids, x);
            await f.add(ids, x);
            const [ha, fa] = await Promise.all([h.searchBatch(q, k), f.searchBatch(q, k)]);
            let hit = 0;
            for (let i = 0; i < nq; i++) {
                const b = new Set(fa.ids.subarray(i * k, i * k + k));
                for (const id of ha.ids.subarray(i * k, i * k + k)) if (b.has(id)) hit++;
            }
            const recall = hit / (nq * k);
            recallLog.push(kind + "/" + metric + "=" + recall.toFixed(4));
            ok(recall >= 0.95, kind + " " + metric + ": recall@10 >= 0.95", recall.toFixed(4));
            const single = await h.search(row(q, dims, 7), k);
            ok(sameIds(single.ids, ha.ids.subarray(70, 80)), kind + " " + metric + ": search equals the searchBatch row");
            h.close();
            f.close();
        }
    }
    print("  recall@10 " + recallLog.join(" "));
});

await __kitS.section("filtered search", async () => {
    const dims = 16, n = 4000, nq = 40, k = 10;
    const x = clusteredData(n, dims, 303), q = clusteredData(nq, dims, 304), ids = seqIds(n);
    const r = rng(305);
    const pick = (c) => { const s = new Set(); while (s.size < c) s.add(Math.floor(r() * n)); return [...s]; };
    const rbOf = (a) => { const b = new RoaringBitmap(); for (const v of a) b.add(v); return b; };
    const bsOf = (a) => { const b = new BitSet(); for (const v of a) b.set(v); return b; };
    for (const metric of METRICS) {
        const h = new VectorIndex({ dims, metric }), hb = new VectorIndex({ dims, metric, dyna: { exactFallback: false } });
        const f = new VectorIndex({ dims, metric, index: "flat" }), bf = new Brute(dims, metric);
        await h.add(ids, x); await hb.add(ids, x); await f.add(ids, x); bf.addAll(ids, x);
        for (const c of [2000, 800]) {
            const allow = pick(c), set = new Set(allow), rb = rbOf(allow);
            let inSet = 0, hit = 0, flatEq = 0;
            for (let i = 0; i < nq; i++) {
                const qi = row(q, dims, i), got = idsOf(await h.search(qi, k, { filter: rb })), want = bf.search(qi, k, (id) => set.has(id));
                if (got.every((id) => set.has(id))) inSet++;
                const ws = new Set(want.ids);
                for (const id of got) if (ws.has(id)) hit++;
                const fl = await f.search(qi, k, { filter: rb });
                if (sameIds(fl.ids, want.ids) && closeF(fl.distances, want.distances)) flatEq++;
            }
            ok(inSet === nq, metric + " card " + c + ": every hit is in the filter");
            ok(hit / (nq * k) >= 0.95, metric + " card " + c + ": filtered beam recall@10 >= 0.95", (hit / (nq * k)).toFixed(4));
            ok(flatEq === nq, metric + " card " + c + ": flat filtered equals brute force", flatEq + "/" + nq);
        }
        const sel = pick(250), selSet = new Set(sel), rb250 = rbOf(sel), bs250 = bsOf(sel), rb251 = rbOf([...sel, ...pick(400).filter((v) => !selSet.has(v)).slice(0, 1)]);
        let exactEq = 0, beamDiff = 0, edgeEq = 0, bsEq = 0;
        for (let i = 0; i < nq; i++) {
            const qi = row(q, dims, i), fl = await f.search(qi, k, { filter: rb250 });
            const g = await h.search(qi, k, { filter: rb250, efSearch: 10 });
            if (sameIds(g.ids, fl.ids)) exactEq++;
            if (!sameIds((await hb.search(qi, k, { filter: rb250, efSearch: 10 })).ids, fl.ids)) beamDiff++;
            if (sameIds((await h.search(qi, k, { filter: rb251, efSearch: 10 })).ids, (await hb.search(qi, k, { filter: rb251, efSearch: 10 })).ids)) edgeEq++;
            if (sameIds((await h.search(qi, k, { filter: bs250, efSearch: 10 })).ids, g.ids)) bsEq++;
        }
        ok(exactEq === nq, metric + ": card*16 == size takes the exact path (equals flat)", exactEq + "/" + nq);
        ok(beamDiff > 0, metric + ": the beam at efSearch 10 differs from exact somewhere (the boundary test discriminates)", String(beamDiff));
        ok(edgeEq === nq, metric + ": card*16 == size+16 runs the filtered beam", edgeEq + "/" + nq);
        ok(bsEq === nq, metric + ": a BitSet filter equals the same RoaringBitmap filter", bsEq + "/" + nq);
    }
    const dim2 = 8, n2 = 4000, x2 = uniformData(n2, dim2, 401), q2 = uniformData(20, dim2, 402);
    const h2 = new VectorIndex({ dims: dim2 }), hb2 = new VectorIndex({ dims: dim2, dyna: { exactFallback: false } }), f2 = new VectorIndex({ dims: dim2, index: "flat" });
    await h2.add(seqIds(n2), x2); await hb2.add(seqIds(n2), x2); await f2.add(seqIds(n2), x2);
    const ghost = new RoaringBitmap();
    for (const v of [3, 5, 9000000, 4294967294]) ghost.add(v);
    for (const idx of [h2, hb2, f2]) {
        const g = idsOf(await idx.search(row(q2, dim2, 0), 10, { filter: ghost }));
        ok(sameIds(g.sort((a, b) => a - b), [3, 5]), idx.index + ": filter ids absent from the index are ignored", g.join());
        ok((await idx.search(row(q2, dim2, 0), 10, { filter: new RoaringBitmap() })).ids.length === 0, idx.index + ": an empty filter returns nothing");
        ok((await idx.search(row(q2, dim2, 0), 10, { filter: new BitSet() })).ids.length === 0, idx.index + ": an empty BitSet returns nothing");
    }
});

await __kitS.section("every vector is reachable by the beam", async () => {
    const dims = 16, n = 1500, x = clusteredData(n, dims, 901), ids = seqIds(n);
    for (const metric of METRICS) {
        const h = new VectorIndex({ dims, metric, dyna: { exactFallback: false } });
        await h.add(ids, x);
        const q = new Float32Array(dims).fill(0.3);
        let miss = 0;
        for (let i = 0; i < n; i++) {
            const b = new BitSet();
            b.set(i);
            if (Number((await h.search(q, 1, { filter: b, efSearch: 2 })).ids[0]) !== i) miss++;
        }
        ok(miss === 0, metric + ": a one-id filter finds every vector through the graph (no orphans)", miss + " unreachable");
    }
});

await __kitS.section("64-bit ids", async () => {
    for (const index of ["flat", "hnsw"]) {
        const v = new VectorIndex({ dims: 2, index });
        const big = [0n, 1n, 4294967295n, 4294967296n, 2n ** 53n + 1n, 2n ** 63n, 2n ** 64n - 2n];
        await v.add(BigUint64Array.from(big), Float32Array.from(big.flatMap((_, i) => [i, 0])));
        ok(v.size === 7 && big.every((b) => v.has(b)), index + ": every 64-bit id is present", String(v.size));
        ok(v.has(4294967295) && !v.has(2 ** 53 + 1) && v.has(2n ** 53n + 1n), index + ": has() takes numbers up to 2^53 and bigints beyond");
        const r = await v.search(Float32Array.of(6, 0), 7);
        ok(r.ids[0] === 2n ** 64n - 2n && r.ids[1] === 2n ** 63n && r.ids[6] === 0n, index + ": ids come back as full 64-bit values", Array.from(r.ids).join());
        await rejects(() => v.add(BigUint64Array.of(2n ** 64n - 1n), Float32Array.of(1, 1)), RangeError, /reserved/, index + ": 2^64-1 is reserved");
        await rejects(() => v.add(BigUint64Array.of(4294967296n), Float32Array.of(1, 1)), RangeError, /already/, index + ": a 64-bit duplicate is caught");
        await rejects(() => v.add(Uint32Array.of(5, 5), Float32Array.of(1, 1, 2, 2)), RangeError, /twice/, index + ": a duplicate within a batch");
        await v.add(Uint32Array.of(4294967295 - 1), Float32Array.of(6, 0));
        const rb = new RoaringBitmap();
        rb.add(1); rb.add(4294967295); rb.add(0);
        const fr = idsOf(await v.search(Float32Array.of(6, 0), 10, { filter: rb })).sort((a, b) => a - b);
        ok(sameIds(fr, [0, 1, 4294967295]), index + ": a filter admits only ids below 2^32", fr.join());
        const wide = new RoaringBitmap();
        for (const id of [0, 1, 4294967295]) wide.add(id);
        for (let i = 100; i < 140; i++) wide.add(i);
        const scanIdx = index === "flat" ? v : null;
        if (scanIdx) {
            const sr = idsOf(await scanIdx.search(Float32Array.of(6, 0), 10, { filter: wide })).sort((a, b) => a - b);
            ok(sameIds(sr, [0, 1, 4294967295]), "flat: a filter larger than the index (scan path) still never admits ids at or above 2^32", sr.join());
        }
        ok(v.remove(BigUint64Array.of(2n ** 63n, 2n ** 63n, 77n)) === 1 && !v.has(2n ** 63n) && v.size === 7, index + ": remove by 64-bit id");
    }
});

await __kitS.section("64-bit ids through the filtered beam", async () => {
    const v = new VectorIndex({ dims: 2, dyna: { exactFallback: false } });
    const big = [0n, 1n, 4294967295n, 4294967296n, 4294967297n, 2n ** 63n];
    await v.add(BigUint64Array.from(big), Float32Array.from(big.flatMap((_, i) => [i, 0])));
    const rb = new RoaringBitmap();
    rb.add(0); rb.add(1);
    const got = idsOf(await v.search(Float32Array.of(4, 0), 10, { filter: rb })).sort((a, b) => a - b);
    ok(sameIds(got, [0, 1]), "the beam never admits ids at or above 2^32 even when their low 32 bits are in the filter", got.join());
});

await __kitS.section("remove and tombstones", async () => {
    const dims = 12, n = 3000, x = clusteredData(n, dims, 1001), q = clusteredData(60, dims, 1002), ids = seqIds(n, 2, 10);
    const r = rng(1003);
    for (const metric of METRICS) {
        const h = new VectorIndex({ dims, metric }), f = new VectorIndex({ dims, metric, index: "flat" }), bf = new Brute(dims, metric);
        await h.add(ids, x); await f.add(ids, x); bf.addAll(ids, x);
        const gone = Uint32Array.from(Array.from(ids).filter(() => r() < 0.3));
        ok(h.remove(gone) === gone.length && f.remove(gone) === gone.length, metric + ": remove returns the number present");
        ok(h.remove(gone) === 0 && h.remove(Uint32Array.of(1, 3, 5)) === 0, metric + ": removing absent ids counts 0");
        for (const g of gone) bf.remove(g);
        const goneSet = new Set(Array.from(gone));
        ok(h.size === n - gone.length && f.size === n - gone.length && !h.has(gone[0]), metric + ": size and has after remove");
        const rb = new RoaringBitmap();
        for (let i = 0; i < n; i += 5) rb.add(ids[i]);
        const rbSmall = new RoaringBitmap();
        for (let i = 0; i < 100; i++) rbSmall.add(ids[i * 7]);
        let flatEq = 0, leaked = 0, hit = 0, fbEq = 0;
        for (let i = 0; i < 60; i++) {
            const qi = row(q, dims, i), want = bf.search(qi, 10);
            const fl = await f.search(qi, 10);
            if (sameIds(fl.ids, want.ids)) flatEq++;
            const hs = idsOf(await h.search(qi, 10)), hf = idsOf(await h.search(qi, 10, { filter: rb })), hx = idsOf(await h.search(qi, 10, { filter: rbSmall }));
            if ([...hs, ...hf, ...hx].some((id) => goneSet.has(id))) leaked++;
            const ws = new Set(want.ids);
            for (const id of hs) if (ws.has(id)) hit++;
            if (sameIds(hx, bf.search(qi, 10, (id) => rbSmall.has(id)).ids)) fbEq++;
        }
        ok(flatEq === 60, metric + ": flat after remove equals brute force over the survivors", flatEq + "/60");
        ok(leaked === 0, metric + ": no removed id comes back (plain, filtered beam, exact fallback)", String(leaked));
        ok(hit / 600 >= 0.95, metric + ": hnsw recall@10 after removing 30% >= 0.95", (hit / 600).toFixed(4));
        ok(fbEq === 60, metric + ": the exact fallback skips tombstones", fbEq + "/60");
        const back = gone.subarray(0, 50), nv = uniformData(50, dims, 1004);
        await h.add(back, nv); await f.add(back, nv);
        for (let i = 0; i < 50; i++) bf.add(back[i], row(nv, dims, i));
        let reEq = 0;
        for (let i = 0; i < 50; i++) {
            const got = idsOf(await f.search(row(nv, dims, i), 1));
            if (got[0] === back[i] || bf.search(row(nv, dims, i), 1).ids[0] === got[0]) reEq++;
        }
        ok(reEq === 50 && h.size === n - gone.length + 50 && h.has(back[0]), metric + ": a removed id may be added again with a new vector", reEq + "/50");
        let reH = 0;
        for (let i = 0; i < 50; i++) if (idsOf(await h.search(row(nv, dims, i), 1))[0] === bf.search(row(nv, dims, i), 1).ids[0]) reH++;
        ok(reH >= 48, metric + ": hnsw finds the re-added vectors", reH + "/50");
    }
    const all = new VectorIndex({ dims: 2 });
    await all.add(Uint32Array.of(1, 2, 3), Float32Array.of(0, 0, 1, 1, 2, 2));
    all.remove(Uint32Array.of(1, 2, 3));
    ok(all.size === 0 && (await all.search(Float32Array.of(0, 0), 3)).ids.length === 0, "an index with every vector removed returns nothing");
    await all.add(Uint32Array.of(2), Float32Array.of(5, 5));
    ok(sameIds((await all.search(Float32Array.of(0, 0), 3)).ids, [2]), "and accepts new vectors after");
});

await __kitS.section("id map churn against a model", async () => {
    const r = rng(1101), v = new VectorIndex({ dims: 1, index: "flat" }), model = new Map();
    const pool = [];
    for (let i = 0; i < 600; i++) pool.push(i % 3 === 0 ? BigInt(i) * 4294967296n + 7n : BigInt(i * 1024));
    let bad = 0;
    for (let round = 0; round < 40; round++) {
        const addIds = [], addVals = [];
        for (const id of pool) if (!model.has(id) && r() < 0.3) { addIds.push(id); addVals.push(Number(id % 1000n)); }
        await v.add(BigUint64Array.from(addIds), Float32Array.from(addVals));
        for (let i = 0; i < addIds.length; i++) model.set(addIds[i], addVals[i]);
        const rm = pool.filter((id) => r() < 0.25);
        let want = 0;
        for (const id of new Set(rm)) if (model.delete(id)) want++;
        if (v.remove(BigUint64Array.from(rm)) !== want) bad++;
        if (v.size !== model.size) bad++;
        for (const id of pool) if (v.has(id) !== model.has(id)) bad++;
    }
    ok(bad === 0, "has/size/remove agree with a Map through 40 rounds of churn", String(bad));
    const res = await v.search(Float32Array.of(0), 1000);
    const got = new Map(Array.from(res.ids, (id, i) => [id, res.distances[i]]));
    let mism = 0;
    for (const [id, val] of model) if (!got.has(id) || got.get(id) !== Math.fround(val * val)) mism++;
    ok(got.size === model.size && mism === 0, "every surviving id maps to its own vector", got.size + " vs " + model.size + ", " + mism + " mismatched");
});

await __kitS.section("determinism and seeds", async () => {
    const dims = 12, n = 1500, x = clusteredData(n, dims, 501), q = clusteredData(50, dims, 502), ids = seqIds(n, 5, 2);
    const results = async (dyna, chunks) => {
        const v = new VectorIndex({ dims, dyna });
        for (let s = 0; s < n; s += chunks) await v.add(ids.subarray(s, s + chunks), x.subarray(s * dims, (s + chunks) * dims));
        const b = await v.searchBatch(q, 8, { efSearch: 8 });
        return Array.from(b.ids).join() + "|" + Array.from(b.distances).join();
    };
    const a = await results({ seed: 42 }, n), b = await results({ seed: 42 }, n), c = await results({ seed: 42 }, 1);
    ok(a === b, "the same seed builds the same index");
    ok(a === c, "one add of n equals n adds of one");
    ok(a !== await results({ seed: 43 }, n), "a different seed builds a different graph (visible at efSearch 8)");
    ok(await results(undefined, n) === await results({ seed: 0 }, n), "the default seed is 0");
    ok((await results({ heuristic: false }, n)).length > 0, "heuristic: false builds and searches");
});

await __kitS.section("searchBatch", async () => {
    const dims = 8, n = 600, nq = 300, x = uniformData(n, dims, 601), q = uniformData(nq, dims, 602);
    for (const index of ["flat", "hnsw"]) {
        const v = new VectorIndex({ dims, index });
        await v.add(seqIds(n), x);
        const rb = new RoaringBitmap();
        for (let i = 0; i < n; i += 3) rb.add(i);
        const qc = q.slice();
        const p = v.searchBatch(qc, 7, { filter: rb });
        throws(() => v.has(1), TypeError, /await it first/, index + ": an offloaded searchBatch is pending");
        qc.fill(0);
        rb.clear();
        const b = await p;
        const rb2 = new RoaringBitmap();
        for (let i = 0; i < n; i += 3) rb2.add(i);
        let eq = 0;
        for (let i = 0; i < nq; i++) {
            const s = await v.search(row(q, dims, i), 7, { filter: rb2 });
            if (b.counts[i] === s.ids.length && sameIds(b.ids.subarray(i * 7, i * 7 + 7), s.ids) && sameIds(b.distances.subarray(i * 7, i * 7 + 7), s.distances)) eq++;
        }
        ok(eq === nq, index + ": rows equal per-query search; queries and filter were copied at call time", eq + "/" + nq);
        const small = new VectorIndex({ dims: 2, index });
        await small.add(Uint32Array.of(4, 6), Float32Array.of(1, 0, 0, 1));
        const pb = await small.searchBatch(Float32Array.of(1, 0, 0, 1), 3);
        const P = 18446744073709551615n;
        ok(sameIds(pb.ids, [4n, 6n, P, 6n, 4n, P].map(String).map(BigInt)) && pb.ids[2] === P && pb.distances[2] === Infinity && pb.distances[5] === Infinity && sameIds(pb.counts, [2, 2]), index + ": rows pad with 2^64-1 / Infinity", Array.from(pb.ids).join());
        const z = await small.searchBatch(new Float32Array(0), 3);
        ok(z.ids.length === 0 && z.distances.length === 0 && z.counts.length === 0, index + ": zero queries");
        await small.add(new Uint32Array(0), new Float32Array(0));
        ok(small.size === 2, index + ": an empty add is a no-op");
    }
});

await __kitS.section("calls run one at a time in call order", async () => {
    const dims = 32, n = 3000, x = uniformData(n, dims, 701);
    const v = new VectorIndex({ dims });
    const pAdd = v.add(seqIds(n), x);
    ok(v.size === 0, "an offloaded add is pending: size still 0", String(v.size));
    throws(() => v.has(1), TypeError, /await it first/, "has while an add is pending");
    throws(() => v.remove(Uint32Array.of(1)), TypeError, /await it first/, "remove while an add is pending");
    throws(() => v.close(), TypeError, /in flight/, "close while an add is pending");
    const pSearch = v.search(row(x, dims, 5), 1);
    const pAdd2 = v.add(Uint32Array.of(99999), new Float32Array(dims).fill(9));
    const pSearch2 = v.search(new Float32Array(dims).fill(9), 1);
    const pDup = v.add(Uint32Array.of(5), new Float32Array(dims));
    ok(v.dims === dims && v.metric === "l2" && v.index === "hnsw", "getters work while pending");
    const order = [];
    await Promise.all([pAdd.then(() => order.push("add")), pSearch.then((r) => order.push("search:" + r.ids[0])), pAdd2.then(() => order.push("add2")), pSearch2.then((r) => order.push("search2:" + r.ids[0])), pDup.then(() => order.push("dup ok"), (e) => order.push("dup " + e.name))]);
    ok(order.join() === "add,search:5,add2,search2:99999,dup RangeError", "settle in call order and each sees the calls before it", order.join());
    ok(v.size === n + 1 && v.has(99999), "both adds landed; the duplicate checked at run time changed nothing");
    const many = await Promise.all(Array.from({ length: 20 }, (_, i) => v.search(row(x, dims, i), 1)));
    ok(many.every((r, i) => Number(r.ids[0]) === i), "concurrent small searches all resolve");
    v.close();
    ok(v.closed, "close after everything settled");
});

await __kitS.section("hostile option getters", async () => {
    const dims = 32, x = uniformData(3000, dims, 801);
    const v = new VectorIndex({ dims });
    await v.add(seqIds(1000), x.subarray(0, 1000 * dims));
    const q = new Float32Array(dims).fill(0.5);
    await rejects(() => v.search(q, 3, { get efSearch() { q.buffer.transfer(); return 10; } }), TypeError, /detached/, "a getter that detaches the query is seen after options are read");
    let pending;
    const target = row(x, dims, 2500);
    const r = await v.search(target, 1, { get efSearch() { pending = v.add(seqIds(2000, 1, 1000), x.subarray(1000 * dims)); return 64; } });
    ok(pending instanceof Promise && Number(r.ids[0]) === 2500, "a getter that starts an add runs first; the search sees it", String(r.ids[0]));
    await pending;
    await rejects(() => v.search(q.slice ? new Float32Array(dims) : q, 3, { get efSearch() { v.close(); return 10; } }), TypeError, /in flight/, "a getter that closes the index is refused");
    ok(!v.closed, "and the index stays open");
    for (let round = 0; round < 50; round++) {
        const rbHit = await v.search(new Float32Array(dims).fill(0.25), 3, { get filter() { const b = new RoaringBitmap(); b.add(7); b.add(900); return b; } });
        const bsHit = await v.search(new Float32Array(dims).fill(0.25), 3, { get filter() { const b = new BitSet(); b.set(7); b.set(900); return b; } });
        if (round === 0) {
            ok(sameIds(idsOf(rbHit).sort((a, b) => a - b), [7, 900]), "a filter returned only by a getter stays alive through the search (RoaringBitmap)", idsOf(rbHit).join());
            ok(sameIds(idsOf(bsHit).sort((a, b) => a - b), [7, 900]), "a filter returned only by a getter stays alive through the search (BitSet)", idsOf(bsHit).join());
        }
    }
});

await __kitS.section("DynResource", async () => {
    const v = new VectorIndex({ dims: 2 });
    await v.add(Uint32Array.of(1), Float32Array.of(1, 1));
    ok(typeof v.close === "function" && typeof v.dispose === "function" && typeof v[Symbol.dispose] === "function", "close, dispose, [Symbol.dispose]");
    v[Symbol.dispose]();
    ok(v.closed === true, "closed after [Symbol.dispose]");
    v.close();
    ok(v.closed, "close is idempotent");
    await rejects(() => v.search(Float32Array.of(1, 1), 1), TypeError, /closed/, "search after close");
    await rejects(() => v.add(Uint32Array.of(2), Float32Array.of(1, 1)), TypeError, /closed/, "add after close");
    await rejects(() => v.searchBatch(Float32Array.of(1, 1), 1), TypeError, /closed/, "searchBatch after close");
    throws(() => v.remove(Uint32Array.of(1)), TypeError, /closed/, "remove after close");
    throws(() => v.has(1), TypeError, /closed/, "has after close");
    throws(() => v.size, TypeError, /closed/, "size after close");
    const w = new VectorIndex({ dims: 2 });
    w.dispose();
    ok(w.closed, "dispose closes");
});

await __kitS.section("errors", async () => {
    throws(() => new VectorIndex(), TypeError, /options/, "no options");
    throws(() => new VectorIndex({}), TypeError, /dims is required/, "dims missing");
    throws(() => new VectorIndex({ dim: 4 }), TypeError, /dim/, "dim (the old name) is an unknown key");
    throws(() => new VectorIndex({ dims: 0 }), RangeError, /dims/, "dims 0");
    throws(() => new VectorIndex({ dims: 65537 }), RangeError, /dims/, "dims 65537");
    throws(() => new VectorIndex({ dims: 2.5 }), RangeError, /dims/, "dims 2.5");
    throws(() => new VectorIndex({ dims: "4" }), TypeError, /dims/, "dims string");
    throws(() => new VectorIndex({ dims: 4, metric: "dot" }), RangeError, /metric/, "bad metric");
    throws(() => new VectorIndex({ dims: 4, metric: 1 }), TypeError, /metric/, "metric not a string");
    throws(() => new VectorIndex({ dims: 4, index: "ivf" }), RangeError, /index must be one of/, "bad index");
    throws(() => new VectorIndex({ dims: 4, index: "ivfpq" }), RangeError, /"ivfpq" is unsupported in this build/, "ivfpq unsupported in this build");
    ok(new VectorIndex({ dims: 4, quantization: "sq8" }).quantization === "sq8" && new VectorIndex({ dims: 4, quantization: "binary" }).quantization === "binary", "sq8 and binary are accepted (see test_vec_vector_index_quantization.js)");
    throws(() => new VectorIndex({ dims: 4, quantization: "f16" }), RangeError, /quantization must be one of/, "bad quantization");
    ok(new VectorIndex({ dims: 4, quantization: "f32" }).size === 0, "quantization f32 is accepted");
    throws(() => new VectorIndex({ dims: 4, m: 1 }), RangeError, /m must/, "m 1");
    throws(() => new VectorIndex({ dims: 4, m: 257 }), RangeError, /m must/, "m 257");
    throws(() => new VectorIndex({ dims: 4, M: 8 }), TypeError, /"M"/, "M (the old name) is an unknown key");
    throws(() => new VectorIndex({ dims: 4, efConstruction: 0 }), RangeError, /efConstruction/, "efConstruction 0");
    throws(() => new VectorIndex({ dims: 4, efSearch: 64 }), TypeError, /efSearch/, "efSearch belongs in dyna");
    throws(() => new VectorIndex({ dims: 4, dyna: 1 }), TypeError, /dyna must be an object/, "dyna not an object");
    throws(() => new VectorIndex({ dims: 4, dyna: { efSearch: 65537 } }), RangeError, /dyna\.efSearch/, "dyna.efSearch 65537");
    throws(() => new VectorIndex({ dims: 4, dyna: { seed: -1 } }), RangeError, /dyna\.seed/, "dyna.seed -1");
    throws(() => new VectorIndex({ dims: 4, dyna: { exactFallback: 1 } }), TypeError, /exactFallback/, "dyna.exactFallback not boolean");
    throws(() => new VectorIndex({ dims: 4, dyna: { exact: true } }), TypeError, /exact/, "unknown dyna key");
    throws(() => new VectorIndex({ dims: 4, index: "flat", m: 8 }), TypeError, /only to index "hnsw"/, "m with flat");
    throws(() => new VectorIndex({ dims: 4, index: "flat", dyna: { heuristic: true } }), TypeError, /only to index "hnsw"/, "dyna with flat");
    ok(new VectorIndex({ dims: 4, m: 2, efConstruction: 1, dyna: { efSearch: 1 } }).size === 0, "minimum parameters are accepted");
    for (const index of ["flat", "hnsw"]) {
        for (const metric of METRICS) {
            const v = new VectorIndex({ dims: 3, index, metric });
            await v.add(Uint32Array.of(1), Float32Array.of(1, 2, 3));
            const t = index + "/" + metric + ": ";
            await rejects(() => v.add(Uint32Array.of(1), Float32Array.of(1, 2, 3)), RangeError, /already/, t + "duplicate id");
            await rejects(() => v.add(Uint32Array.of(2), Float32Array.of(1, 2)), RangeError, /ids.length \* dims/, t + "length mismatch on add");
            await rejects(() => v.add([2], Float32Array.of(1, 2, 3)), TypeError, /BigUint64Array or Uint32Array/, t + "plain array ids");
            await rejects(() => v.add(Int32Array.of(2), Float32Array.of(1, 2, 3)), TypeError, /BigUint64Array or Uint32Array/, t + "Int32Array ids");
            await rejects(() => v.add(Uint32Array.of(2), Float64Array.of(1, 2, 3)), TypeError, /Float32Array/, t + "Float64Array vectors");
            await rejects(() => v.add(Uint32Array.of(2), Float32Array.of(1, NaN, 3)), RangeError, /NaN/, t + "NaN vector");
            await rejects(() => v.add(Uint32Array.of(2, 3), Float32Array.of(1, 1, 1, 1, Infinity, 1)), RangeError, /NaN or Infinity/, t + "Infinity vector");
            if (metric === "cosine") await rejects(() => v.add(Uint32Array.of(2), Float32Array.of(0, 0, 0)), RangeError, /zero vector/, t + "zero vector");
            else { await v.add(Uint32Array.of(2), Float32Array.of(0, 0, 0)); ok(v.has(2), t + "zero vector is fine"); }
            await rejects(() => v.add(Uint32Array.of(50, 51, 1), Float32Array.of(1, 1, 1, 2, 2, 2, 3, 3, 3)), RangeError, /already/, t + "a batch ending in an id already present");
            ok(!v.has(50) && !v.has(51), t + "that batch inserted nothing (checked before the first insert)");
            const before = v.size;
            ok(!v.has(3) && v.size === before, t + "rejected adds left the index unchanged");
            await rejects(() => v.search(Float32Array.of(1, 2), 1), RangeError, /length 2/, t + "dims mismatch on search");
            await rejects(() => v.search(Float32Array.of(NaN, 2, 3), 1), RangeError, /NaN/, t + "NaN query");
            if (metric === "cosine") await rejects(() => v.search(Float32Array.of(0, 0, 0), 1), RangeError, /zero vector/, t + "zero query");
            await rejects(() => v.search(Float32Array.of(1, 2, 3), 0), RangeError, /k/, t + "k 0");
            await rejects(() => v.search(Float32Array.of(1, 2, 3), 65537), RangeError, /k/, t + "k 65537");
            await rejects(() => v.search(Float32Array.of(1, 2, 3), 1.5), RangeError, /k/, t + "k 1.5");
            await rejects(() => v.search(Float32Array.of(1, 2, 3)), TypeError, /k/, t + "k missing");
            await rejects(() => v.search(Float32Array.of(1, 2, 3), 1, { efSearch: 0 }), RangeError, /efSearch/, t + "efSearch 0");
            await rejects(() => v.search(Float32Array.of(1, 2, 3), 1, { ef: 4 }), TypeError, /"ef"/, t + "unknown search key");
            await rejects(() => v.search(Float32Array.of(1, 2, 3), 1, { filter: new Set([1]) }), TypeError, /RoaringBitmap or BitSet/, t + "Set filter");
            await rejects(() => v.search(Float32Array.of(1, 2, 3), 1, { filter: null }), TypeError, /RoaringBitmap or BitSet/, t + "null filter");
            await rejects(() => v.search(Float32Array.of(1, 2, 3), 1, 5), TypeError, /options/, t + "options not an object");
            await rejects(() => v.search([1, 2, 3], 1), TypeError, /Float32Array/, t + "plain array query");
            await rejects(() => v.searchBatch(new Float32Array(4), 1), RangeError, /multiple of dims/, t + "searchBatch length");
            await rejects(() => v.searchBatch(Float32Array.of(1, 2, 3, Infinity, 1, 1), 1), RangeError, /NaN or Infinity/, t + "searchBatch non-finite query");
            throws(() => v.has("1"), TypeError, /number or a bigint/, t + "has string");
            throws(() => v.remove([1]), TypeError, /BigUint64Array or Uint32Array/, t + "remove plain array");
            const ab = new ArrayBuffer(12), det = new Float32Array(ab);
            ab.transfer();
            await rejects(() => v.add(Uint32Array.of(7), det), TypeError, /detached/, t + "detached vectors");
            ok(v.has(1) && !v.closed, t + "index intact after every rejection");
        }
    }
    await rejects(() => VectorIndex.prototype.search.call({}, Float32Array.of(1, 2), 1), TypeError, null, "search on a foreign receiver");
});

__kitS.end();
print(`test_vec_vector_index: ${pass} passed, ${fail} failed`);
if (fail) throw new Error("test_vec_vector_index: " + fail + " failing checks");
