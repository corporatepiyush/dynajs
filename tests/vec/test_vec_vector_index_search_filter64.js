// timeout: 300
// covers: dyna:vec.VectorIndex.search
// The 64-bit id filter: search(q, k, { filter: BigUint64Array }) with the array sorted strictly ascending.
// Chosen over a new Roaring64 class because ids already travel as BigUint64Array, a sorted array answers
// membership by binary search with no build step, and its length is the cardinality the exact fallback needs.
// Oracle: a brute-force scan keyed by BigInt ids (Number would merge ids above 2^53) over vectors stored the way
// the contract says. Data mixes uint32 ids with ids at the 2^32 boundary, above 2^53 and at 2^64-2, so every
// path is checked on both sides of the uint32 domain:
//   flat scan (filter larger than the live set), flat enumerate (filter smaller), hnsw exact fallback
//   (card <= ef and card * 16 <= size), and the filtered beam with dyna.exactFallback false.
// Flat and the fallback must equal brute force exactly; the beam must keep every hit inside the filter and reach
// recall@10 >= 0.95. Also: tombstoned ids in the filter never return, ids absent from the index are ignored, an
// empty array returns nothing, unsorted or duplicated arrays reject with RangeError, the array is copied at call
// time (a queued search ignores later writes to it), searchBatch takes the same filter, and RoaringBitmap/BitSet
// keep refusing ids >= 2^32. The fallback boundary (card * 16 == size takes the exact path, one more id takes the
// beam) is pinned on a deliberately weak graph (m 2, efConstruction 8, no heuristic) so the beam at efSearch 10
// provably differs from the exact answer for every metric.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_vec_vector_index_search_filter64");
import { VectorIndex } from "dyna:vec";
import { RoaringBitmap } from "dyna:structures";

let pass = 0, fail = 0;
const ok = (c, w, d) => { if (c) pass++; else { fail++; print("  FAIL  " + w + (d ? "  [" + d + "]" : "")); } };
async function rejects(p, Ctor, re, w) {
    let r;
    try { r = p(); } catch (e) { ok(false, w, "threw synchronously: " + e.message); return; }
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

function data(n, dim, seed) {
    const r = rng(seed), x = new Float32Array(n * dim);
    for (let i = 0; i < x.length; i++) x[i] = r() * 2 - 1;
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

function brute(rows, metric, q, k, allow) {
    const qs = store(metric, q), out = [];
    for (const [id, x] of rows) if (allow.has(id)) out.push([dist(metric, qs, x), id]);
    out.sort((a, b) => a[0] - b[0] || (a[1] < b[1] ? -1 : a[1] > b[1] ? 1 : 0));
    return out.slice(0, k).map((t) => t[1]);
}

const big = (a) => Array.from(a, (v) => BigInt(v));
const same = (a, b) => a.length === b.length && a.every((v, i) => v === b[i]);
const sorted = (s) => BigUint64Array.from([...s].sort((a, b) => (a < b ? -1 : a > b ? 1 : 0)));
const row = (x, dim, i) => x.subarray(i * dim, (i + 1) * dim);
const METRICS = ["l2", "cosine", "ip"];

function mixedIds(n, seed) {
    const r = rng(seed), s = new Set();
    const fixed = [0n, 1n, 4294967294n, 4294967295n, 4294967296n, 4294967297n, 2n ** 53n + 1n, 2n ** 63n, 2n ** 64n - 2n];
    for (const v of fixed) s.add(v);
    while (s.size < n) {
        const u = BigInt(Math.floor(r() * 4294967296));
        s.add(r() < 0.5 ? u : (u << 32n) | BigInt(Math.floor(r() * 4294967296)));
    }
    const out = [...s];
    for (let i = out.length - 1; i > 0; i--) { const j = Math.floor(r() * (i + 1)); [out[i], out[j]] = [out[j], out[i]]; }
    return BigUint64Array.from(out);
}

await __kitS.section("every search path honours a 64-bit filter", async () => {
    const dims = 8, n = 3000, nq = 30, k = 10;
    const ids = mixedIds(n, 71), x = data(n, dims, 72), q = data(nq, dims, 73), r = rng(74);
    ok(ids.some((v) => v > 4294967295n) && ids.some((v) => v <= 4294967295n), "data mixes 32-bit and 64-bit ids");
    const pick = (c) => { const s = new Set(); while (s.size < c) s.add(ids[Math.floor(r() * n)]); return s; };
    for (const metric of METRICS) {
        const rows = new Map();
        for (let i = 0; i < n; i++) rows.set(ids[i], store(metric, row(x, dims, i)));
        const flat = new VectorIndex({ dims, metric, index: "flat" });
        const hnsw = new VectorIndex({ dims, metric });
        const beam = new VectorIndex({ dims, metric, dyna: { exactFallback: false } });
        for (const ix of [flat, hnsw, beam]) await ix.add(ids, x);
        const cases = [
            { c: 1500, what: "half the ids (flat scan, hnsw beam)" },
            { c: 150, what: "150 ids (flat enumerate, hnsw fallback: card*16 <= size)" },
            { c: 40, what: "40 ids (hnsw fallback: card <= ef)" },
        ];
        for (const { c, what } of cases) {
            const allow = pick(c), f = sorted(allow);
            const extra = sorted(new Set([...allow, 4294967296n + 77777n, 2n ** 64n - 3n]));
            let flatEq = 0, hnswIn = 0, hit = 0, exactEq = 0, beamIn = 0, beamHit = 0;
            for (let i = 0; i < nq; i++) {
                const qi = row(q, dims, i), want = brute(rows, metric, qi, k, allow), ws = new Set(want);
                const fl = big((await flat.search(qi, k, { filter: f })).ids);
                if (same(fl, want) && same(big((await flat.search(qi, k, { filter: extra })).ids), want)) flatEq++;
                const h = big((await hnsw.search(qi, k, { filter: f })).ids);
                if (h.every((v) => allow.has(v))) hnswIn++;
                for (const v of h) if (ws.has(v)) hit++;
                if (same(h, want)) exactEq++;
                const b = big((await beam.search(qi, k, { filter: f })).ids);
                if (b.every((v) => allow.has(v))) beamIn++;
                for (const v of b) if (ws.has(v)) beamHit++;
            }
            ok(flatEq === nq, metric + " " + what + ": flat equals brute force (ids absent from the index ignored)", flatEq + "/" + nq);
            ok(hnswIn === nq && beamIn === nq, metric + " " + what + ": every hnsw hit is in the filter");
            if (c * 16 <= n || c <= 128) ok(exactEq === nq, metric + " " + what + ": the exact fallback equals brute force", exactEq + "/" + nq);
            ok(hit / (nq * k) >= 0.95, metric + " " + what + ": hnsw recall@10 >= 0.95", (hit / (nq * k)).toFixed(4));
            ok(beamHit / (nq * k) >= 0.95, metric + " " + what + ": filtered beam recall@10 >= 0.95", (beamHit / (nq * k)).toFixed(4));
        }
        const fAll = sorted(new Set([...ids, 5n, 4294967296n + 99n]));
        let scanEq = 0;
        for (let i = 0; i < nq; i++) {
            const qi = row(q, dims, i), want = big((await flat.search(qi, k)).ids);
            if (same(big((await flat.search(qi, k, { filter: fAll })).ids), want)) scanEq++;
        }
        ok(scanEq === nq, metric + ": a filter covering every id (flat scan path) equals no filter", scanEq + "/" + nq);
        const thin = new VectorIndex({ dims, metric, index: "flat" });
        await thin.add(ids, x);
        const gone = BigUint64Array.from(ids.subarray(0, 2000));
        ok(thin.remove(gone) === 2000, metric + ": thin out to 1000 live vectors");
        const goneSet = new Set(gone), allow = pick(1500), f = sorted(allow), live = new Set([...allow].filter((v) => !goneSet.has(v)));
        let thinEq = 0;
        for (let i = 0; i < nq; i++) {
            const qi = row(q, dims, i);
            if (same(big((await thin.search(qi, k, { filter: f })).ids), brute(rows, metric, qi, k, live))) thinEq++;
        }
        ok(thinEq === nq, metric + ": card 1500 over 1000 live (flat scan path with a subset) equals brute force", thinEq + "/" + nq);
        thin.close();
        const hi = sorted(new Set([...ids].filter((v) => v > 4294967295n)));
        let onlyHi = 0;
        for (let i = 0; i < nq; i++) {
            const qi = row(q, dims, i);
            const all = [flat, hnsw, beam].map(async (ix) => big((await ix.search(qi, k, { filter: hi })).ids));
            const got = await Promise.all(all);
            if (got.every((g) => g.length === k && g.every((v) => v > 4294967295n))) onlyHi++;
        }
        ok(onlyHi === nq, metric + ": a filter of only ids >= 2^32 returns k of them on every path", onlyHi + "/" + nq);
        const rb = new RoaringBitmap();
        for (const v of ids) if (v <= 4294967295n) rb.add(Number(v));
        const viaRb = big((await flat.search(row(q, dims, 0), 50, { filter: rb })).ids);
        ok(viaRb.length === 50 && viaRb.every((v) => v <= 4294967295n), metric + ": a RoaringBitmap still never passes ids >= 2^32");
        for (const ix of [flat, hnsw, beam]) ix.close();
    }
});

await __kitS.section("boundary ids, tombstones and absent ids", async () => {
    for (const index of ["flat", "hnsw"]) {
        for (const exactFallback of index === "hnsw" ? [true, false] : [true]) {
            const opts = index === "hnsw" ? { dims: 2, index, dyna: { exactFallback } } : { dims: 2, index };
            const ix = new VectorIndex(opts), t = index + (exactFallback ? "" : " beam") + ": ";
            const ids = BigUint64Array.of(4294967295n, 4294967296n, 0n, 2n ** 64n - 2n, 7n);
            await ix.add(ids, Float32Array.of(1, 0, 2, 0, 3, 0, 4, 0, 5, 0));
            const q = Float32Array.of(0, 0);
            let r = big((await ix.search(q, 5, { filter: BigUint64Array.of(4294967295n, 4294967296n) })).ids);
            ok(same(r, [4294967295n, 4294967296n]), t + "2^32-1 and 2^32 both pass", r.join());
            r = big((await ix.search(q, 5, { filter: BigUint64Array.of(0n, 2n ** 64n - 2n) })).ids);
            ok(same(r, [0n, 2n ** 64n - 2n]), t + "0 and 2^64-2 pass", r.join());
            r = big((await ix.search(q, 5, { filter: BigUint64Array.of(1n, 4294967297n, 2n ** 64n - 1n) })).ids);
            ok(r.length === 0, t + "ids absent from the index (and 2^64-1) match nothing", r.join());
            ok((await ix.search(q, 5, { filter: new BigUint64Array(0) })).ids.length === 0, t + "an empty array returns nothing");
            ok(ix.remove(BigUint64Array.of(4294967296n)) === 1, t + "remove a 64-bit id");
            r = big((await ix.search(q, 5, { filter: BigUint64Array.of(4294967295n, 4294967296n, 2n ** 64n - 2n) })).ids);
            ok(same(r, [4294967295n, 2n ** 64n - 2n]), t + "a tombstoned id in the filter never returns", r.join());
            await ix.add(BigUint64Array.of(4294967296n), Float32Array.of(0.5, 0));
            r = big((await ix.search(q, 1, { filter: BigUint64Array.of(4294967296n) })).ids);
            ok(same(r, [4294967296n]), t + "a re-added 64-bit id passes again", r.join());
            const b = await ix.searchBatch(Float32Array.of(0, 0, 10, 0), 2, { filter: BigUint64Array.of(7n, 2n ** 64n - 2n) });
            ok(same(big(b.ids), [2n ** 64n - 2n, 7n, 7n, 2n ** 64n - 2n]) && same(Array.from(b.counts), [2, 2]), t + "searchBatch takes the same filter", big(b.ids).join());
            ix.close();
        }
    }
});

await __kitS.section("the exact-fallback boundary with a 64-bit filter", async () => {
    const dims = 16, n = 4000, nq = 40, k = 10, r = rng(81);
    const ids = mixedIds(n, 82), x = new Float32Array(n * dims), q = new Float32Array(nq * dims), cent = new Float32Array(40 * dims);
    for (let i = 0; i < cent.length; i++) cent[i] = r() * 2 - 1;
    const fill = (out, m) => { for (let i = 0; i < m; i++) { const c = Math.floor(r() * 40), s = 0.5 + r(); for (let d = 0; d < dims; d++) out[i * dims + d] = (cent[c * dims + d] + 0.08 * Math.sqrt(-2 * Math.log(1 - r())) * Math.cos(2 * Math.PI * r())) * s; } };
    fill(x, n); fill(q, nq);
    const pick = (c) => { const s = new Set(); while (s.size < c) s.add(ids[Math.floor(r() * n)]); return s; };
    for (const metric of METRICS) {
        const weak = { m: 2, efConstruction: 8 };
        const h = new VectorIndex({ dims, metric, ...weak, dyna: { heuristic: false } }), hb = new VectorIndex({ dims, metric, ...weak, dyna: { heuristic: false, exactFallback: false } }), f = new VectorIndex({ dims, metric, index: "flat" });
        for (const ix of [h, hb, f]) await ix.add(ids, x);
        const s250 = pick(250), f250 = sorted(s250);
        let extraId = 0n;
        for (const v of ids) if (!s250.has(v)) { extraId = v; break; }
        const f251 = sorted(new Set([...s250, extraId]));
        let exactEq = 0, beamDiff = 0, edgeEq = 0;
        for (let i = 0; i < nq; i++) {
            const qi = row(q, dims, i), fl = big((await f.search(qi, k, { filter: f250 })).ids);
            if (same(big((await h.search(qi, k, { filter: f250, efSearch: 10 })).ids), fl)) exactEq++;
            if (!same(big((await hb.search(qi, k, { filter: f250, efSearch: 10 })).ids), fl)) beamDiff++;
            if (same(big((await h.search(qi, k, { filter: f251, efSearch: 10 })).ids), big((await hb.search(qi, k, { filter: f251, efSearch: 10 })).ids))) edgeEq++;
        }
        ok(exactEq === nq, metric + ": card*16 == size takes the exact path (equals flat)", exactEq + "/" + nq);
        ok(beamDiff > 0, metric + ": the beam at efSearch 10 differs from exact somewhere (the boundary discriminates)", String(beamDiff));
        ok(edgeEq === nq, metric + ": card*16 == size+16 runs the filtered beam", edgeEq + "/" + nq);
        for (const ix of [h, hb, f]) ix.close();
    }
});

await __kitS.section("the filter array: order, copy, errors", async () => {
    const D = 32, vec = (v) => { const a = new Float32Array(D); a[0] = v; return a; };
    const ix = new VectorIndex({ dims: D });
    const four = new Float32Array(4 * D);
    for (let i = 0; i < 4; i++) four[i * D] = i + 1;
    await ix.add(BigUint64Array.of(1n, 2n, 3n, 2n ** 40n), four);
    const q = vec(0);
    await rejects(() => ix.search(q, 2, { filter: BigUint64Array.of(3n, 1n) }), RangeError, /sorted ascending with no duplicates/, "descending filter rejects");
    await rejects(() => ix.search(q, 2, { filter: BigUint64Array.of(1n, 2n ** 40n, 2n ** 40n) }), RangeError, /sorted ascending with no duplicates/, "duplicate 64-bit id rejects");
    await rejects(() => ix.searchBatch(vec(0), 2, { filter: BigUint64Array.of(2n, 2n) }), RangeError, /sorted ascending/, "searchBatch duplicate rejects");
    await rejects(() => ix.search(q, 2, { filter: Uint32Array.of(1, 2) }), TypeError, /sorted BigUint64Array, or a RoaringBitmap or BitSet/, "a Uint32Array filter is a TypeError");
    await rejects(() => ix.search(q, 2, { filter: BigInt64Array.of(1n) }), TypeError, /sorted BigUint64Array/, "a BigInt64Array filter is a TypeError");
    const ab = new ArrayBuffer(16), det = new BigUint64Array(ab);
    ab.transfer();
    await rejects(() => ix.search(q, 2, { filter: det }), TypeError, /filter is detached/, "a detached filter is a TypeError");
    const sub = new BigUint64Array([0n, 2n, 2n ** 40n, 9n]).subarray(1, 3);
    ok(same(big((await ix.search(q, 4, { filter: sub })).ids), [2n, 2n ** 40n]), "a subarray view filters by its own window");
    const f = BigUint64Array.of(1n, 2n);
    const bulk = new Float32Array(3000 * D).map((_, i) => 100 + (i % 7));
    const p0 = ix.add(BigUint64Array.from({ length: 3000 }, (_, i) => 1000n + BigInt(i)), bulk);
    ok(ix.size === 4, "a large add is pending, so the next calls queue");
    const p1 = ix.add(BigUint64Array.of(50n), vec(0));
    const p2 = ix.search(q, 4, { filter: f });
    f[0] = 3n; f[1] = 50n;
    await p0; await p1;
    ok(same(big((await p2).ids), [1n, 2n]), "a queued search uses the filter as it was at the call");
    ok(same(big((await ix.search(q, 4, { filter: f })).ids), [50n, 3n]), "a later search sees the new contents");
    ix.close();
});

__kitS.end();
print(`test_vec_vector_index_search_filter64: ${pass} passed, ${fail} failed`);
if (fail) throw new Error("test_vec_vector_index_search_filter64: " + fail + " failing checks");
