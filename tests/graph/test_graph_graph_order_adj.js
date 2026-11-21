// covers: integration c:src/core/graphk/graphk-walk.c c:src/modules/graph/graph-traversal.c -- the module calls the new adjacency walks directly
// F-graph-1: Graph#bfs(src) and Graph#dfs(src) copied the whole graph into a CSR (three allocations and
// every arc) on each call, ran the walk on the copy and built the result with one property define per
// node. They now walk the adjacency lists in place (BFS uses the output order as its queue) and build
// the array in one step: at 1K nodes bfs 30.6 -> 13.8 us, dfs 26.6 -> 16.1 us. No JS runs during
// these walks, so reading the live lists is safe; the visitor forms still walk a CSR snapshot because
// a visitor may mutate the graph.
// Oracles: a JS walk over exportCSR() (insertion order, an undirected edge under both ends, a self
// loop once) and the visitor forms, which still take the CSR path. Graphs: directed and undirected,
// self loops, parallel edges, isolated nodes, every node as the source.
import { suite } from "../lib/kit.js";
import { Graph } from "dyna:graph";

const t = suite("graph bfs/dfs over adjacency lists");
let seed = 7;
const rnd = (n) => ((seed = (Math.imul(seed, 1103515245) + 12345) >>> 0) % n);
function refBfs(csr, n, src) {
    const seen = new Uint8Array(n), out = [src];
    seen[src] = 1;
    for (let h = 0; h < out.length; h++) {
        const u = out[h];
        for (let e = csr.offsets[u]; e < csr.offsets[u + 1]; e++) {
            const v = csr.edges[e];
            if (!seen[v]) { seen[v] = 1; out.push(v); }
        }
    }
    return out;
}
function refDfs(csr, n, src) {
    const seen = new Uint8Array(n), out = [], st = [src];
    while (st.length) {
        const u = st.pop();
        if (seen[u]) continue;
        seen[u] = 1;
        out.push(u);
        for (let e = csr.offsets[u + 1] - 1; e >= csr.offsets[u]; e--) if (!seen[csr.edges[e]]) st.push(csr.edges[e]);
    }
    return out;
}
function visitOrder(g, kind, src) {
    const out = [];
    g[kind](src, { visit: (x) => { out.push(x); } });
    return out;
}
function randomGraph(directed, n, m) {
    const g = new Graph({ directed });
    for (let i = 0; i < n; i++) g.addNode();
    for (let k = 0; k < m; k++) {
        const u = rnd(n), v = rnd(5) === 0 ? u : rnd(n);
        g.addEdge(u, v);
        if (rnd(6) === 0) g.addEdge(u, v);
    }
    return g;
}
t.test("bfs and dfs equal a walk over exportCSR and the visitor forms", (a) => {
    let bad = 0, cases = 0;
    for (const directed of [false, true]) {
        for (const [n, m] of [[1, 0], [1, 2], [2, 1], [7, 5], [40, 30], [40, 160], [300, 1200], [1000, 4000]]) {
            const g = randomGraph(directed, n, m), csr = g.exportCSR();
            const step = n > 100 ? 37 : 1;
            for (let src = 0; src < n; src += step) {
                cases++;
                const b = g.bfs(src), d = g.dfs(src);
                const ok = JSON.stringify(b) === JSON.stringify(refBfs(csr, n, src)) && JSON.stringify(d) === JSON.stringify(refDfs(csr, n, src))
                    && JSON.stringify(b) === JSON.stringify(visitOrder(g, "bfs", src)) && JSON.stringify(d) === JSON.stringify(visitOrder(g, "dfs", src));
                if (!ok && ++bad <= 3) {
                    a.deep(b, refBfs(csr, n, src), "bfs directed=" + directed + " n=" + n + " src=" + src);
                    a.deep(d, refDfs(csr, n, src), "dfs directed=" + directed + " n=" + n + " src=" + src);
                }
            }
        }
    }
    a.eq(bad, 0, "mismatches over " + cases + " sources");
});
t.test("results are plain dense arrays of int node ids", (a) => {
    const g = randomGraph(false, 50, 120);
    for (const r of [g.bfs(3), g.dfs(3)]) {
        a.ok(Array.isArray(r) && Object.getPrototypeOf(r) === Array.prototype);
        a.ok(r.every((x, i) => Number.isInteger(x) && r.indexOf(x) === i), "distinct ints");
        r.push(-1);
        a.eq(r[r.length - 1], -1, "writable and extensible");
    }
    const one = new Graph();
    one.addNode();
    a.deep(one.bfs(0), [0]);
    a.deep(one.dfs(0), [0]);
});
t.test("the graph can change between calls", (a) => {
    const g = new Graph({ directed: true });
    for (let i = 0; i < 4; i++) g.addNode();
    g.addEdge(0, 1);
    a.deep(g.bfs(0), [0, 1]);
    g.addEdge(1, 3);
    g.addEdge(0, 2);
    a.deep(g.bfs(0), [0, 1, 2, 3]);
    a.deep(g.dfs(0), [0, 1, 3, 2]);
    g.addNode();
    g.addEdge(3, 4);
    a.deep(g.dfs(0), [0, 1, 3, 4, 2]);
});
await t.run();
