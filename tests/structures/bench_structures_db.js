// Native vs pure-JS cost of the structures added for interpreter-bound code.
// Each row runs the same workload twice -- once on the dyna:structures class,
// once on the tightest plain-JS equivalent (binary search over sorted arrays,
// typed-array heaps, Map/Set) -- checks that both produce the same answer, and
// reports best-of-3 milliseconds. A row whose answers differ is a failure, not
// a measurement. Run: ./dynajs tests/structures/bench_structures_db.js
import { ZSet, OrderedMap, RoaringBitmap, TDigest, TopK, CuckooFilter, IndexedHeap, KDTree, ConsistentHash, InvertedIndex, Interner } from "dyna:structures";

const rows = [];
let bad = 0;
function rng(seed) { let s = seed >>> 0 || 1; return () => { s ^= s << 13; s >>>= 0; s ^= s >>> 17; s ^= s << 5; s >>>= 0; return s / 4294967296; }; }
function best(fn) { let b = Infinity, r; for (let i = 0; i < 3; i++) { const t0 = performance.now(); r = fn(); const d = performance.now() - t0; if (d < b) b = d; } return [b, r]; }
function row(name, nat, js) {
    const [tn, rn] = best(nat), [tj, rj] = best(js);
    const ok = JSON.stringify(rn) === JSON.stringify(rj);
    if (!ok) { bad++; print("MISMATCH " + name + "\n  native " + JSON.stringify(rn).slice(0, 200) + "\n  js     " + JSON.stringify(rj).slice(0, 200)); }
    rows.push([name, tn, tj]);
    print(`${name.padEnd(46)} native ${tn.toFixed(1).padStart(8)} ms   js ${tj.toFixed(1).padStart(9)} ms   ${(tj / tn).toFixed(1).padStart(6)}x${ok ? "" : "  MISMATCH"}`);
}
function lowerBound(a, x, n = a.length) { let lo = 0, hi = n; while (lo < hi) { const m = (lo + hi) >> 1; if (a[m] < x) lo = m + 1; else hi = m; } return lo; }

// 1. Leaderboard: 50k players, 200k score updates, then rank queries and the top 10.
{
    const P = 50000, U = 200000, r = rng(1), ids = [], deltas = [];
    for (let i = 0; i < U; i++) { ids.push("p" + Math.floor(r() * P)); deltas.push(Math.floor(r() * 100)); }
    row("ZSet leaderboard 200k incr + 20k ranks + top10", () => {
        const z = new ZSet();
        for (let i = 0; i < U; i++) z.incrBy(ids[i], deltas[i]);
        let s = 0; for (let i = 0; i < 20000; i++) s += z.revRank(ids[i]) ?? 0;
        return [s, z.range(0, 9, { reverse: true })];
    }, () => {
        const score = new Map(), keys = [], vals = [];
        const cmp = (s1, m1, s2, m2) => s1 - s2 || (m1 < m2 ? -1 : m1 > m2 ? 1 : 0);
        const find = (s, m) => { let lo = 0, hi = keys.length; while (lo < hi) { const mid = (lo + hi) >> 1; if (cmp(vals[mid], keys[mid], s, m) < 0) lo = mid + 1; else hi = mid; } return lo; };
        for (let i = 0; i < U; i++) {
            const m = ids[i], old = score.get(m);
            if (old !== undefined) { const j = find(old, m); keys.splice(j, 1); vals.splice(j, 1); }
            const ns = (old ?? 0) + deltas[i]; score.set(m, ns);
            const j = find(ns, m); keys.splice(j, 0, m); vals.splice(j, 0, ns);
        }
        let s = 0; for (let i = 0; i < 20000; i++) { const m = ids[i]; s += keys.length - 1 - find(score.get(m), m); }
        const top = []; for (let i = keys.length - 1; i >= keys.length - 10; i--) top.push([keys[i], vals[i]]);
        return [s, top];
    });
}
// 2. Ordered string map: 100k inserts in random order, 10k prefix scans of 20.
{
    const N = 100000, r = rng(2), keys = []; for (let i = 0; i < N; i++) keys.push("user/" + Math.floor(r() * 1e9).toString(36));
    const pref = []; for (let i = 0; i < 10000; i++) pref.push("user/" + Math.floor(r() * 36).toString(36));
    row("OrderedMap 100k set + 10k prefix(20)", () => {
        const o = new OrderedMap(); for (let i = 0; i < N; i++) o.set(keys[i], i);
        let s = 0; for (const p of pref) s += o.prefix(p, { limit: 20 }).length; return s;
    }, () => {
        const m = new Map(), sorted = [];
        for (let i = 0; i < N; i++) { const k = keys[i]; if (!m.has(k)) { const j = lowerBound(sorted, k); sorted.splice(j, 0, k); } m.set(k, i); }
        let s = 0; for (const p of pref) { let j = lowerBound(sorted, p), c = 0; while (j < sorted.length && c < 20 && sorted[j].startsWith(p)) { c++; j++; } s += c; } return s;
    });
}
// 3. Bitmap index: two 1M-row predicates, AND + count + 1k membership probes.
{
    const r = rng(3), a = [], b = [];
    for (let i = 0; i < 2000000; i++) { if (r() < 0.5) a.push(i); if (r() < 0.3) b.push(i); }
    const A = Uint32Array.from(a), B = Uint32Array.from(b);
    row("RoaringBitmap AND of 1M x 600k rows", () => {
        const x = new RoaringBitmap(A), y = new RoaringBitmap(B);
        return x.andCardinality(y);
    }, () => {
        let c = 0, i = 0, j = 0; while (i < A.length && j < B.length) { if (A[i] < B[j]) i++; else if (A[i] > B[j]) j++; else { c++; i++; j++; } } return c;
    });
    const x = new RoaringBitmap(A), y = new RoaringBitmap(B), sa = new Set(a), sb = new Set(b);
    row("RoaringBitmap prebuilt AND vs JS Set filter", () => x.andCardinality(y), () => { let c = 0; for (const v of sa) if (sb.has(v)) c++; return c; });
}
// 4. Streaming percentiles: 1M latencies, p50/p90/p99/p999.
{
    const r = rng(4), N = 1000000, xs = new Float64Array(N); for (let i = 0; i < N; i++) xs[i] = Math.exp(r() * 5);
    const qs = [0.5, 0.9, 0.99, 0.999];
    const exact = Float64Array.from(xs).sort();
    row("TDigest 1M values -> 4 quantiles (within 1%)", () => {
        const t = new TDigest(200); t.addMany(xs);
        return qs.map(q => Math.abs(t.quantile(q) / exact[Math.floor(q * (N - 1))] - 1) < 0.01);
    }, () => {
        const s = Float64Array.from(xs).sort(); return qs.map(q => s[Math.floor(q * (N - 1))] > 0);
    });
    row("TDigest 1M one-by-one add + p99", () => { const t = new TDigest(); for (let i = 0; i < N; i++) t.add(xs[i]); return t.quantile(0.99) > 0; },
        () => { const a = []; for (let i = 0; i < N; i++) a.push(xs[i]); a.sort((p, q) => p - q); return a[Math.floor(0.99 * (N - 1))] > 0; });
}
// 5. Heavy hitters: 1M events over a Zipf vocabulary of 100k. Space-Saving only
//    guarantees keys above N/k, so k is sized for the 0.5% keys of interest.
{
    const r = rng(5), N = 1000000, ev = []; for (let i = 0; i < N; i++) ev.push("url" + Math.floor(100000 * r() ** 3));
    row("TopK(500) over 1M events (top-5 keys)", () => { const t = new TopK(500); for (let i = 0; i < N; i++) t.add(ev[i]); return t.list().slice(0, 5).map(e => e[0]); },
        () => { const m = new Map(); for (let i = 0; i < N; i++) { const k = ev[i]; m.set(k, (m.get(k) || 0) + 1); } return [...m].sort((p, q) => q[1] - p[1] || (p[0] < q[0] ? -1 : 1)).slice(0, 5).map(e => e[0]); });
}
// 6. Membership: 200k keys, 400k probes (Set is native too; the filter's win is memory).
{
    const N = 200000, keys = [], probes = []; for (let i = 0; i < N; i++) keys.push("k" + i); for (let i = 0; i < 2 * N; i++) probes.push("k" + (i * 3));
    row("CuckooFilter 200k add + 400k probes", () => { const c = new CuckooFilter(N); for (const k of keys) c.add(k); let h = 0; for (const p of probes) if (c.mayContain(p)) h++; return h > 66000 && h < 67000; },
        () => { const s = new Set(keys); let h = 0; for (const p of probes) if (s.has(p)) h++; return h > 66000 && h < 67000; });
}
// 7. Dijkstra with decrease-key on a 300x300 grid.
{
    const W = 300, r = rng(7), w = new Float64Array(W * W).map(() => 1 + Math.floor(r() * 9));
    const nb = (u, f) => { const x = u % W, y = (u / W) | 0; if (x > 0) f(u - 1); if (x < W - 1) f(u + 1); if (y > 0) f(u - W); if (y < W - 1) f(u + W); };
    row("IndexedHeap Dijkstra 90k nodes", () => {
        const d = new Float64Array(W * W).fill(Infinity), q = new IndexedHeap(); d[0] = 0; q.push(0, 0);
        while (q.size) { const u = q.pop(); nb(u, v => { const nd = d[u] + w[v]; if (nd < d[v]) { d[v] = nd; q.push(v, nd); } }); }
        return d[W * W - 1];
    }, () => {
        const d = new Float64Array(W * W).fill(Infinity), heap = new Int32Array(W * W), pos = new Int32Array(W * W).fill(-1); let n = 0;
        const less = (a, b) => d[heap[a]] < d[heap[b]] || (d[heap[a]] === d[heap[b]] && heap[a] < heap[b]);
        const swap = (a, b) => { const t = heap[a]; heap[a] = heap[b]; heap[b] = t; pos[heap[a]] = a; pos[heap[b]] = b; };
        const up = (i) => { while (i) { const p = (i - 1) >> 1; if (!less(i, p)) break; swap(i, p); i = p; } };
        const down = (i) => { for (;;) { const l = 2 * i + 1, rr = l + 1; let m = i; if (l < n && less(l, m)) m = l; if (rr < n && less(rr, m)) m = rr; if (m === i) return; swap(i, m); i = m; } };
        d[0] = 0; heap[0] = 0; pos[0] = 0; n = 1;
        while (n) { const u = heap[0]; pos[u] = -1; n--; if (n) { heap[0] = heap[n]; pos[heap[0]] = 0; down(0); }
            nb(u, v => { const nd = d[u] + w[v]; if (nd < d[v]) { d[v] = nd; if (pos[v] < 0) { heap[n] = v; pos[v] = n; n++; } up(pos[v]); } }); }
        return d[W * W - 1];
    });
}
// 8. Nearest neighbours: 50k points in 3-D, 5k queries, k = 10.
{
    const r = rng(8), N = 50000, Q = 5000, P = new Float64Array(N * 3).map(() => r() * 1000), q = new Float64Array(Q * 3).map(() => r() * 1000);
    const sumIdx = (a) => { let s = 0; for (const v of a) s += v; return s; };
    row("KDTree build + 5k kNN(10) over 50k pts", () => { const t = new KDTree(P, 3); return sumIdx(t.knnMany(q, 10).indices); }, () => {
        let s = 0; const bd = new Float64Array(10), bi = new Int32Array(10);
        for (let j = 0; j < Q; j++) {
            bd.fill(Infinity); bi.fill(-1);
            const x = q[3 * j], y = q[3 * j + 1], z = q[3 * j + 2];
            for (let i = 0; i < N; i++) { const dx = P[3 * i] - x, dy = P[3 * i + 1] - y, dz = P[3 * i + 2] - z, dd = dx * dx + dy * dy + dz * dz;
                if (dd < bd[9]) { let k = 9; while (k > 0 && bd[k - 1] > dd) { bd[k] = bd[k - 1]; bi[k] = bi[k - 1]; k--; } bd[k] = dd; bi[k] = i; } }
            for (let k = 0; k < 10; k++) s += bi[k];
        }
        return s;
    });
}
// 9. Sharding: 16 nodes x 160 replicas, 200k key lookups (JS ring with FNV-1a).
{
    const keys = []; for (let i = 0; i < 200000; i++) keys.push("session:" + i);
    const nodes = []; for (let i = 0; i < 16; i++) nodes.push("node-" + i);
    const fnv = (s) => { let h = 0x811c9dc5; for (let i = 0; i < s.length; i++) { h ^= s.charCodeAt(i); h = Math.imul(h, 0x01000193) >>> 0; }
        h ^= h >>> 16; h = Math.imul(h, 0x85ebca6b) >>> 0; h ^= h >>> 13; h = Math.imul(h, 0xc2b2ae35) >>> 0; h ^= h >>> 16; return h >>> 0; };
    row("ConsistentHash 200k lookups (balance check)", () => {
        const c = new ConsistentHash(); for (const n of nodes) c.addNode(n);
        const cnt = new Map(); for (const k of keys) { const o = c.get(k); cnt.set(o, (cnt.get(o) || 0) + 1); } return Math.max(...cnt.values()) < 200000 / 16 * 1.25;
    }, () => {
        const pts = []; for (const n of nodes) for (let r = 0; r < 160; r++) pts.push([fnv(n + "#" + r), n]); pts.sort((a, b) => a[0] - b[0]);
        const hs = Uint32Array.from(pts, p => p[0]);
        const cnt = new Map(); for (const k of keys) { let j = lowerBound(hs, fnv(k)); if (j === hs.length) j = 0; const o = pts[j][1]; cnt.set(o, (cnt.get(o) || 0) + 1); } return Math.max(...cnt.values()) < 200000 / 16 * 1.25;
    });
}
// 10. Full-text search: 20k docs, 2k BM25 queries, top 10.
{
    const r = rng(10), vocab = []; for (let i = 0; i < 5000; i++) vocab.push("w" + i.toString(36));
    const docs = []; for (let i = 0; i < 20000; i++) { const len = 20 + Math.floor(r() * 60), ws = []; for (let j = 0; j < len; j++) ws.push(vocab[Math.floor(5000 * r() ** 2)]); docs.push(ws.join(" ")); }
    const queries = []; for (let i = 0; i < 2000; i++) queries.push(vocab[Math.floor(5000 * r() ** 2)] + " " + vocab[Math.floor(5000 * r() ** 2)]);
    row("InvertedIndex 20k docs + 2k BM25 queries", () => {
        const ii = new InvertedIndex(); docs.forEach((d, i) => ii.add(i, d));
        let s = 0; for (const q of queries) { const h = ii.search(q, { limit: 10 }); if (h.length) s += h[0][0]; } return s;
    }, () => {
        const post = new Map(), len = []; let total = 0;
        docs.forEach((d, i) => { const ws = d.split(" "); len.push(ws.length); total += ws.length; const tf = new Map(); for (const w of ws) tf.set(w, (tf.get(w) || 0) + 1); for (const [w, c] of tf) { let p = post.get(w); if (!p) post.set(w, p = []); p.push(i, c); } });
        const N = docs.length, avg = total / N, acc = new Float64Array(N), stamp = new Int32Array(N); let epoch = 0, s = 0;
        for (const q of queries) {
            epoch++; const touched = [];
            for (const t of new Set(q.split(" "))) { const p = post.get(t); if (!p) continue; const df = p.length / 2, idf = Math.log(1 + (N - df + 0.5) / (df + 0.5));
                for (let k = 0; k < p.length; k += 2) { const di = p[k], tf = p[k + 1]; if (stamp[di] !== epoch) { stamp[di] = epoch; acc[di] = 0; touched.push(di); } acc[di] += idf * tf * 2.2 / (tf + 1.2 * (0.25 + 0.75 * len[di] / avg)); } }
            let bi = -1, bs = -1; for (const di of touched) if (acc[di] > bs || (acc[di] === bs && di < bi)) { bs = acc[di]; bi = di; }
            if (bi >= 0) s += bi;
        }
        return s;
    });
}
// 11. Dictionary encoding: 1M strings over 50k distinct values -> ids.
{
    const r = rng(11), col = []; for (let i = 0; i < 1000000; i++) col.push("city-" + Math.floor(r() * 50000));
    row("Interner.internMany 1M strings (50k distinct)", () => { const it = new Interner(); const ids = it.internMany(col); return [it.size, ids[999999]]; },
        () => { const m = new Map(), ids = new Uint32Array(col.length); for (let i = 0; i < col.length; i++) { const s = col[i]; let id = m.get(s); if (id === undefined) { id = m.size; m.set(s, id); } ids[i] = id; } return [m.size, ids[999999]]; });
}

const geo = Math.exp(rows.reduce((s, [, tn, tj]) => s + Math.log(tj / tn), 0) / rows.length);
print(`geometric-mean speedup over ${rows.length} rows: ${geo.toFixed(1)}x`);
if (bad) throw new Error(bad + " rows computed different answers");
