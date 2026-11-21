// covers: dyna:structures.CSRGraph.dijkstra
// dyn_csr_dijkstra fills dist (+Inf) and pred (-1) in two split loops over a hoisted n (memset_pattern16 /
// memset) into malloc'd, uninitialised buffers: every unreachable node, the last one included, must read
// Infinity and -1. The oracle is Bellman-Ford in JS.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_structures_csr_graph_dijkstra_init_oracle");
import { CSRGraph } from "dyna:structures";

let checks = 0, fails = 0;
function ok(c, what) { checks++; if (!c) { fails++; print("FAIL: " + what); } }
let seed = 0x5EED;
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

__kitS.section("unreachable nodes read Infinity and -1; reachable ones match Bellman-Ford", () => {
    for (const directed of [true, false])
        for (const n of [1, 2, 3, 5, 8, 17, 64, 255, 1000]) {
            const reach = Math.max(1, (n / 3) | 0), edges = [];
            for (let i = 0; i < reach * 3; i++) edges.push([(rnd() * reach) | 0, (rnd() * reach) | 0, 1 + ((rnd() * 9) | 0)]);
            const want = bellmanFord(n, edges, directed, 0), r = new CSRGraph(n, edges, { directed }).dijkstra(0);
            let distOk = true, predOk = true;
            for (let v = 0; v < n; v++) {
                if (r.dist[v] !== want[v]) distOk = false;
                if (want[v] === Infinity || v === 0) { if (r.prev[v] !== -1) predOk = false; }
                else if (!(r.prev[v] >= 0 && r.prev[v] < n)) predOk = false;
            }
            ok(distOk, "dist n=" + n + " directed=" + directed);
            ok(predOk, "prev n=" + n + " directed=" + directed);
        }
});

__kitS.section("a source in a small island of a large graph leaves the rest at Infinity and -1", () => {
    const n = 100000, edges = [];
    for (let i = 0; i < 5; i++) edges.push([n - 6 + i, n - 5 + i, 2]);
    const r = new CSRGraph(n, edges, { directed: true }).dijkstra(n - 6);
    let inf = 0, neg = 0;
    for (let v = 0; v < n; v++) { if (r.dist[v] === Infinity) inf++; if (r.prev[v] === -1) neg++; }
    ok(inf === n - 6, "unreachable count " + inf);
    ok(neg === n - 5, "prev -1 count " + neg);
    ok(r.dist[n - 1] === 10 && r.prev[n - 1] === n - 2, "island tail dist " + r.dist[n - 1]);
});

if (fails) throw new Error(fails + " of " + checks + " checks failed");
print("test_structures_csr_graph_dijkstra_init_oracle: " + checks + " checks passed");
__kitS.end();
