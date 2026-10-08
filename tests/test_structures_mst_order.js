// Graph.mst sorts its edges with a stable radix sort on the weight, so among
// equal weights the edges keep the order they were collected in: node u
// ascending, then u's adjacency order (edges with v >= u only). The result is
// the same on every platform; with qsort it depended on the C library's tie
// handling. The oracle is Kruskal written here over the same collection order
// with JavaScript's stable Array.prototype.sort.
import { Graph } from "dyna:structures";

let n = 0, fails = 0;
function ok(c, m, extra) { n++; if (!c) { fails++; print("FAIL: " + m + (extra !== undefined ? " -- " + extra : "")); } }
let seed = 31;
const rnd = () => (seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0) / 4294967296;

function reference(N, adj) {
    const es = [];
    for (let u = 0; u < N; u++) for (const [v, w] of adj[u]) if (v >= u) es.push([u, v, w]);
    es.sort((a, b) => a[2] - b[2]);
    const parent = Array.from({ length: N }, (_, i) => i);
    const find = (x) => { while (parent[x] !== x) { parent[x] = parent[parent[x]]; x = parent[x]; } return x; };
    const out = [];
    let total = 0;
    for (const [a, b, w] of es) {
        const ra = find(a), rb = find(b);
        if (ra === rb) continue;
        parent[ra] = rb;
        total += w;
        out.push([a, b, w]);
    }
    return { weight: total, edges: out };
}

function build(N, M, weights) {
    const g = new Graph({ weighted: true });
    const adj = Array.from({ length: N }, () => []);
    const seen = new Set();
    for (let i = 0; i < N; i++) g.addNode();
    for (let i = 0; i < M; i++) {
        const u = Math.floor(rnd() * N), v = Math.floor(rnd() * N);
        const k = Math.min(u, v) + ":" + Math.max(u, v);
        if (u === v || seen.has(k)) continue;
        seen.add(k);
        const w = weights();
        g.addEdge(u, v, w);
        adj[u].push([v, w]);
        adj[v].push([u, w]);
    }
    return { g, adj };
}

const WEIGHTS = {
    "all equal": () => 1,
    "few distinct": () => 1 + Math.floor(rnd() * 3),
    "negative and zero": () => [-2, -0.5, 0, -0, 0.5, 2][Math.floor(rnd() * 6)],
    "wide range": () => (rnd() - 0.5) * 10 ** Math.floor(rnd() * 20 - 10),
};
for (const [label, wf] of Object.entries(WEIGHTS)) {
    for (const [N, M] of [[10, 30], [60, 200], [500, 3000]]) {
        const { g, adj } = build(N, M, wf);
        const got = g.mst(), want = reference(N, adj);
        ok(JSON.stringify(got.edges) === JSON.stringify(want.edges), label + " N=" + N + ": same edges in the same order",
            JSON.stringify(got.edges).slice(0, 120) + " vs " + JSON.stringify(want.edges).slice(0, 120));
        ok(Math.abs(got.weight - want.weight) <= 1e-9 * Math.max(1, Math.abs(want.weight)), label + " N=" + N + ": same weight");
    }
}

print("test_structures_mst_order: " + n + " checks, " + fails + " failures");
if (fails) throw new Error(fails + " failures");
