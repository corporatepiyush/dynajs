// covers: dyna:graph.Graph.shortestPathTree
// dyn_gk_dijkstra fills dist (+Inf) and pred (-1) for every node in two split loops over a hoisted n (clang
// turns them into memset_pattern16 / memset); the malloc'd arrays are otherwise uninitialised, so every
// unreachable node must read dist Infinity and pred -1, including the last node and graphs of every size.
// dijkstra(src) runs the same code with pred NULL (no pred fill). The oracle is Bellman-Ford in JS.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_graph_graph_shortest_path_tree_init_oracle");
import { Graph } from "dyna:graph";

let checks = 0, fails = 0;
function ok(c, what) { checks++; if (!c) { fails++; print("FAIL: " + what); } }
let seed = 0xBADC0DE;
const rnd = () => { seed ^= seed << 13; seed ^= seed >>> 17; seed ^= seed << 5; seed >>>= 0; return seed / 4294967296; };
function bellmanFord(n, edges, directed, src) {
    const d = new Array(n).fill(Infinity);
    d[src] = 0;
    for (let it = 0; it < n; it++) {
        let ch = false;
        for (const [u, v, w] of edges) {
            if (d[u] + w < d[v]) { d[v] = d[u] + w; ch = true; }
            if (!directed && d[v] + w < d[u]) { d[u] = d[v] + w; ch = true; }
        }
        if (!ch) break;
    }
    return d;
}
function build(n, reach, directed) {
    const g = new Graph({ weighted: true, directed }), edges = [];
    for (let i = 0; i < n; i++) g.addNode();
    for (let i = 0; i < reach * 3; i++) {
        const u = (rnd() * reach) | 0, v = (rnd() * reach) | 0, w = 1 + ((rnd() * 9) | 0);
        g.addEdge(u, v, w); edges.push([u, v, w]);
    }
    return { g, edges };
}

__kitS.section("unreachable nodes read Infinity and -1; reachable ones match Bellman-Ford", () => {
    for (const directed of [true, false])
        for (const n of [1, 2, 3, 5, 8, 17, 64, 255, 1000]) {
            const reach = Math.max(1, (n / 3) | 0), { g, edges } = build(n, reach, directed);
            const want = bellmanFord(n, edges, directed, 0), t = g.shortestPathTree(0), d = g.dijkstra(0);
            let distOk = true, predOk = true, dOnly = true;
            for (let v = 0; v < n; v++) {
                if (t.dist[v] !== want[v]) distOk = false;
                if (d[v] !== want[v]) dOnly = false;
                if (want[v] === Infinity || v === 0) { if (t.pred[v] !== -1) predOk = false; }
                else if (!(t.pred[v] >= 0 && t.pred[v] < n)) predOk = false;
            }
            ok(distOk, "shortestPathTree dist n=" + n + " directed=" + directed);
            ok(predOk, "shortestPathTree pred n=" + n + " directed=" + directed);
            ok(dOnly, "dijkstra(src) dist n=" + n + " directed=" + directed);
            ok(t.dist[n - 1] === want[n - 1] && (want[n - 1] !== Infinity || t.pred[n - 1] === -1), "last node n=" + n);
        }
});

__kitS.section("a source in a small island of a large graph leaves the rest at Infinity and -1", () => {
    const n = 100000, g = new Graph({ weighted: true, directed: true });
    for (let i = 0; i < n; i++) g.addNode();
    for (let i = 0; i < 5; i++) g.addEdge(n - 6 + i, n - 5 + i, 2);
    const t = g.shortestPathTree(n - 6);
    let inf = 0, neg = 0;
    for (let v = 0; v < n; v++) { if (t.dist[v] === Infinity) inf++; if (t.pred[v] === -1) neg++; }
    ok(inf === n - 6, "unreachable count " + inf);
    ok(neg === n - 5, "pred -1 count " + neg);
    ok(t.dist[n - 1] === 10 && t.pred[n - 1] === n - 2, "island tail dist " + t.dist[n - 1]);
});

if (fails) throw new Error(fails + " of " + checks + " checks failed");
print("test_structures_graph_shortest_path_tree_init_oracle: " + checks + " checks passed");
__kitS.end();
