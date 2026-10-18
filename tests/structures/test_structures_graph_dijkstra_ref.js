// Graph#dijkstra (which now runs on a per-call CSR copy of the adjacency) against a Bellman-Ford
// written here over the same edge list: random directed and undirected graphs with zero weights,
// fractional weights, parallel edges, self loops and unreachable nodes, both call forms, and the
// negative-weight refusal (which the copy loop raises itself).
import { Graph } from "dyna:structures";

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; print("FAIL: " + m); } };
let seed = 7;
const rnd = (n) => { seed ^= seed << 13; seed >>>= 0; seed ^= seed >>> 17; seed ^= seed << 5; seed >>>= 0; return seed % n; };

function bellmanFord(n, edges, directed, src) {
    const d = new Array(n).fill(Infinity);
    d[src] = 0;
    for (let r = 0; r < n; r++) {
        let changed = false;
        for (const [u, v, w] of edges) {
            if (d[u] + w < d[v]) { d[v] = d[u] + w; changed = true; }
            if (!directed && d[v] + w < d[u]) { d[u] = d[v] + w; changed = true; }
        }
        if (!changed) break;
    }
    return d;
}

for (let t = 0; t < 300; t++) {
    const n = 1 + rnd(50), m = rnd(160), directed = rnd(2) === 0;
    const g = new Graph({ directed, weighted: true }), edges = [];
    for (let i = 0; i < n; i++) g.addNode(i);
    for (let i = 0; i < m; i++) {
        const e = [rnd(n), rnd(n), rnd(4) === 0 ? 0 : 1 + rnd(30) + rnd(8) / 8];
        edges.push(e);
        g.addEdge(e[0], e[1], e[2]);
    }
    const src = rnd(n), want = bellmanFord(n, edges, directed, src), got = g.dijkstra(src);
    let bad = 0;
    for (let v = 0; v < n; v++) if (got[v] !== want[v]) bad++;
    ok(bad === 0 && got.length === n, `graph ${t}: every distance equals Bellman-Ford (${bad} differ)`);
    const dst = rnd(n);
    ok(g.dijkstra(src, dst) === want[dst], `graph ${t}: the single-destination form`);
}

{
    const g = new Graph({ directed: true, weighted: true });
    for (let i = 0; i < 3; i++) g.addNode(i);
    g.addEdge(0, 1, 2);
    g.addEdge(1, 2, -0.5);
    let err = null;
    try { g.dijkstra(0); } catch (e) { err = e; }
    ok(err instanceof RangeError && /negative edge weight/.test(err.message), "a negative weight anywhere is refused");
    let err2 = null;
    try { g.dijkstra(5); } catch (e) { err2 = e; }
    ok(err2 instanceof RangeError, "a source out of range is refused");
    const one = new Graph({ directed: true, weighted: true });
    one.addNode(0);
    ok(one.dijkstra(0).length === 1 && one.dijkstra(0)[0] === 0, "a single node is distance 0 from itself");
}

print(`test_structures_graph_dijkstra_ref: ${pass} passed, ${fail} failed`);
if (fail) throw new Error("test_structures_graph_dijkstra_ref failed");
