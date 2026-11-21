// flags: --std
// covers: dyna:structures.CSRGraph
// RBTree, WBTree, SkipList, XArray, PatriciaTrie, CSRGraph, RStarTree,
// SuffixArray: every query is checked against a brute-force model written here
// (a sorted array, a linear scan, Bellman-Ford, naive suffix sort).
import * as std from "std";
import { memoryUsage, setNativeMemoryLimit } from "dyna:sys";
import { CRC32C } from "dyna:hash";
import { suite } from "../lib/kit.js";
import { Deque, RBTree, WBTree, SkipList, XArray, PatriciaTrie, CSRGraph, RStarTree, SuffixArray } from "dyna:structures";

const t = suite("structures_ext");

let seed = 0x2545F491;
function rnd(n) {
    seed ^= seed << 13; seed ^= seed >>> 17; seed ^= seed << 5; seed >>>= 0;
    return seed % n;
}

const enc = new TextEncoder();
function utf8cmp(a, b) {
    const x = enc.encode(a), y = enc.encode(b), n = Math.min(x.length, y.length);
    for (let i = 0; i < n; i++) if (x[i] !== y[i]) return x[i] - y[i];
    return x.length - y.length;
}

function treeModel(cmp) {
    const keys = [], vals = new Map();
    const find = (k) => { let lo = 0, hi = keys.length; while (lo < hi) { const mid = (lo + hi) >> 1; if (cmp(keys[mid], k) < 0) lo = mid + 1; else hi = mid; } return lo; };
    return {
        set(k, v) { const i = find(k); if (i >= keys.length || cmp(keys[i], k) !== 0) keys.splice(i, 0, k); vals.set(k, v); },
        del(k) { const i = find(k); if (i < keys.length && cmp(keys[i], k) === 0) { keys.splice(i, 1); vals.delete(k); return true; } return false; },
        get(k) { return vals.get(k); },
        rank(k) { return find(k); },
        floor(k) { const i = find(k); if (i < keys.length && cmp(keys[i], k) === 0) return keys[i]; return i > 0 ? keys[i - 1] : undefined; },
        ceil(k) { const i = find(k); return i < keys.length ? keys[i] : undefined; },
        keys,
    };
}

function bellmanFord(n, edges, directed, src) {
    const d = new Array(n).fill(Infinity);
    d[src] = 0;
    for (let r = 0; r < n; r++)
        for (const [u, v, w = 1] of edges) {
            if (d[u] + w < d[v]) d[v] = d[u] + w;
            if (!directed && d[v] + w < d[u]) d[u] = d[v] + w;
        }
    return d;
}

t.test("CSRGraph BFS, Dijkstra, components against naive references", (a) => {
    for (const directed of [true, false]) {
        const n = 60, edges = [];
        for (let i = 0; i < 150; i++) edges.push([rnd(n), rnd(n), 1 + rnd(9)]);
        const g = new CSRGraph(n, edges, { directed });
        a.eq(g.nodes, n);
        a.eq(g.edgeCount, directed ? edges.length : edges.length * 2);
        const bf = bellmanFord(n, edges, directed, 0);
        const dj = g.dijkstra(0);
        let bad = 0;
        for (let v = 0; v < n; v++) {
            if (dj.dist[v] !== bf[v]) bad++;
            if (Number.isFinite(bf[v]) && v !== 0) {
                const u = dj.prev[v];
                const ok = edges.some(([x, y, w]) => (x === u && y === v || !directed && x === v && y === u) && bf[u] + w === bf[v]);
                if (!ok) bad++;
            }
        }
        a.eq(bad, 0, "dijkstra (directed=" + directed + ")");
        const hops = bellmanFord(n, edges.map(([u, v]) => [u, v, 1]), directed, 0);
        const bfs = g.bfs(0);
        a.deep([...bfs], hops.map((h) => (Number.isFinite(h) ? h : -1)), "bfs");
        const parent = Array.from({ length: n }, (_, i) => i);
        const find = (x) => (parent[x] === x ? x : (parent[x] = find(parent[x])));
        for (const [u, v] of edges) parent[find(u)] = find(v);
        const roots = new Set(parent.map((_, i) => find(i)));
        const comp = g.components();
        a.eq(comp.count, roots.size, "weak component count");
        let split = 0;
        for (const [u, v] of edges) if (comp.labels[u] !== comp.labels[v]) split++;
        a.eq(split, 0, "every edge stays inside one label");
        let deg = 0;
        for (let v = 0; v < n; v++) { deg += g.degree(v); a.eq(g.neighbors(v).length, g.weights(v).length); }
        a.eq(deg, g.edgeCount);
    }
});

t.test("CSRGraph pagerank matches a power iteration", (a) => {
    const n = 7, edges = [[0, 1], [1, 2], [2, 0], [3, 2], [4, 3], [4, 5], [1, 6]];
    const g = new CSRGraph(n, edges);
    const pr = g.pagerank({ damping: 0.85, iterations: 1000, tolerance: 0 });
    const out = new Array(n).fill(0);
    for (const [u] of edges) out[u]++;
    let r = new Array(n).fill(1 / n);
    for (let it = 0; it < 1000; it++) {
        let dangling = 0;
        for (let v = 0; v < n; v++) if (!out[v]) dangling += r[v];
        const nx = new Array(n).fill((1 - 0.85) / n + 0.85 * dangling / n);
        for (const [u, v] of edges) nx[v] += 0.85 * r[u] / out[u];
        r = nx;
    }
    for (let v = 0; v < n; v++) a.near(pr[v], r[v], 1e-9, "rank of " + v);
    a.near(pr.reduce((s, x) => s + x, 0), 1, 1e-12);
});

t.test("CSRGraph refusals", (a) => {
    a.throws(() => new CSRGraph(3, [[0, 3]]), RangeError);
    a.throws(() => new CSRGraph(3, [[0, 1, NaN]]), RangeError);
    a.throws(() => new CSRGraph(-1, []), RangeError);
    a.throws(() => new CSRGraph(2 ** 26 + 1, []), RangeError, "the node count is bounded");
    a.throws(() => new CSRGraph(3, {}), TypeError);
    a.throws(() => new CSRGraph(3, [5]), TypeError);
    const g = new CSRGraph(2, [[0, 1, -1]]);
    a.throws(() => g.dijkstra(0), RangeError);
    a.throws(() => g.bfs(2), RangeError);
    a.throws(() => g.pagerank({ damping: 2 }), RangeError);
    const e = new CSRGraph(0, []);
    a.eq(e.components().count, 0);
    a.eq(e.pagerank().length, 0);
});

await t.run();
