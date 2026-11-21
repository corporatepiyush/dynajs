// covers: contract dyna:graph.Graph.shortestPath dyna:graph.Graph.shortestPathTree dyna:graph.Graph.criticalPath -- documented values, tie rules and errors
// Independent of the reference vectors: CLRS 3rd ed. Fig. 24.6 (Dijkstra: s,y,t,x at 9) and Fig. 24.5's DAG
// run as a longest path (r,s,t,x,z = 15); the deterministic tie rules (smallest predecessor id, smallest end id);
// dag_longest_path's restart on a negative prefix; empty, unweighted and unreachable cases; every error. Also the
// guarantees of the CSR move: walks run on a private snapshot, so a visitor that mutates the graph still gets
// "the visitor mutated the graph", and aStar refuses a negative weight before it calls the heuristic.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_graph_graph_paths_contract");
import { Graph } from "dyna:graph";

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; print("FAIL " + m); } };
function throws(fn, type, re, msg) {
    try { fn(); ok(false, msg + " (did not throw)"); }
    catch (e) { ok(e instanceof type && re.test(e.message), `${msg} (${e.name}: ${e.message})`); }
}
const graph = (directed, weighted, n, edges) => {
    const g = new Graph({ directed, weighted });
    for (let i = 0; i < n; i++) g.addNode();
    for (const [u, v, w] of edges) g.addEdge(u, v, w);
    return g;
};
const J = (x) => JSON.stringify(x);

__kitS.section("CLRS 24.6: s -> x is s,y,t,x at 9", () => {
    const [S, T, X, Y, Z] = [0, 1, 2, 3, 4];
    const g = graph(true, true, 5, [[S, T, 10], [S, Y, 5], [T, X, 1], [T, Y, 2], [Y, T, 3], [Y, X, 9], [Y, Z, 2], [X, Z, 4], [Z, X, 6], [Z, S, 7]]);
    ok(J(g.shortestPath(S, X)) === J({ dist: 9, path: [S, Y, T, X] }), "CLRS 24.6: s -> x is s,y,t,x at 9");
    const t = g.shortestPathTree(S);
    ok(J([...t.dist]) === J([0, 8, 9, 5, 7]) && J([...t.pred]) === J([-1, Y, T, S, Y]), `CLRS 24.6 tree (${t.dist} / ${t.pred})`);
    ok(J(g.shortestPath(X, X)) === J({ dist: 0, path: [X] }), "a node reaches itself at 0");
    const h = graph(true, true, 3, [[0, 1, 1]]);
    ok(J(h.shortestPath(0, 2)) === J({ dist: null, path: [] }) && h.shortestPath(0, 2).dist === Infinity, "unreachable: Infinity and an empty path");
    ok(h.shortestPathTree(0).pred[2] === -1 && h.shortestPathTree(0).dist[2] === Infinity, "unreachable: pred -1");
    const u = graph(false, false, 4, [[0, 1], [1, 2], [2, 3], [0, 3, 99]]);
    ok(J(u.shortestPath(0, 3)) === J({ dist: 1, path: [0, 3] }), "unweighted graphs count hops (the weight 99 is ignored)");
    const p = graph(true, true, 3, [[0, 1, 5], [0, 1, 1], [1, 2, 1]]);
    ok(p.shortestPath(0, 2).dist === 2, "parallel edges: the lighter one");
});
__kitS.section("parallel edges: the heavier one", () => {
    const g = graph(true, true, 6, [[0, 1, 5], [0, 2, 3], [1, 2, 2], [1, 3, 6], [2, 3, 7], [2, 4, 4], [2, 5, 2], [3, 4, -1], [3, 5, 1], [4, 5, -2]]);
    const cp = g.criticalPath();
    ok(cp.length === 15 && J(cp.path) === J([0, 1, 2, 3, 5]) && J([...cp.dist]) === J([0, 5, 7, 14, 13, 15]), `CLRS 24.5 DAG longest path r,s,t,x,z = 15 (${J(cp)})`);
    const ties = graph(true, true, 7, [[2, 3, 1], [1, 3, 1], [0, 4, 1], [5, 6, 0]]).criticalPath();
    ok(J(ties.path) === J([1, 3]) && ties.length === 1, `ties: smallest predecessor, then smallest end id (${J(ties)})`);
    const neg = graph(true, true, 5, [[0, 1, -5], [1, 2, 1], [3, 4, 2]]).criticalPath();
    ok(neg.length === 2 && J(neg.path) === J([3, 4]) && neg.dist[1] === 0 && neg.dist[2] === 1, `a negative prefix restarts the path (${J(neg)})`);
    const zero = graph(true, true, 3, [[0, 1, 0], [1, 2, 0]]).criticalPath();
    ok(zero.length === 0 && J(zero.path) === J([0]), `all-zero chain: length 0, the smallest node (${J(zero)})`);
    const nz = graph(true, true, 3, [[0, 1, -1], [1, 2, 1]]).criticalPath();
    ok(J(nz.path) === J([1, 2]), `an exactly-zero prefix after a restart extends (${J(nz)})`);
    const par = graph(true, true, 2, [[0, 1, 1], [0, 1, 4]]).criticalPath();
    ok(par.length === 4, "parallel edges: the heavier one");
    const unw = graph(true, false, 5, [[0, 1], [1, 2], [0, 2], [3, 4]]).criticalPath();
    ok(unw.length === 2 && J(unw.path) === J([0, 1, 2]), "unweighted: length counts edges (networkx default_weight 1)");
    const empty = new Graph({ directed: true }).criticalPath();
    ok(empty.length === 0 && empty.path.length === 0 && empty.dist instanceof Float64Array && empty.dist.length === 0, "an empty graph");
    const iso = graph(true, true, 3, []).criticalPath();
    ok(iso.length === 0 && J(iso.path) === J([0]), "isolated nodes: length 0 at node 0");
});
__kitS.section("bfs visitor that adds an edge", () => {
    const g = graph(true, false, 7, [[0, 1], [0, 2], [1, 3], [2, 3], [3, 4], [5, 6]]);
    const seen = [];
    throws(() => g.bfs(0, { visit: (v) => { seen.push(v); if (v === 1) g.addEdge(4, 0); } }), TypeError, /bfs: the visitor mutated the graph/, "bfs visitor that adds an edge");
    ok(J(seen) === J([0, 1]), `the walk stopped at the mutating visit (${seen})`);
    throws(() => g.dfs(0, { visit: () => { g.addNode(); } }), TypeError, /dfs: the visitor mutated the graph/, "dfs visitor that adds a node");
    const d = graph(true, false, 3, [[0, 1], [1, 2]]);
    throws(() => d.topologicalSort({ visit: () => { d.addEdge(2, 0); } }), TypeError, /topologicalSort: the visitor mutated the graph/, "topo visitor that closes a cycle");
    throws(() => d.topologicalSort(), RangeError, /graph has a cycle/, "the cycle the visitor added is seen by the next call");
    ok(d.bfs(0, { visit: (v) => v === 1 }) === 2, "a visitor returning true stops the walk");
});
__kitS.section("aStar with a negative weight", () => {
    let calls = 0;
    const g = graph(true, true, 3, [[0, 1, 1], [1, 2, -1]]);
    throws(() => g.aStar(0, 2, () => { calls++; return 0; }), RangeError, /aStar: negative edge weight/, "aStar with a negative weight");
    ok(calls === 0, "the heuristic is not called when a weight is negative");
    const big = new Graph({ directed: true });
    for (let i = 0; i < 1025; i++) big.addNode();
    throws(() => big.floydWarshall(), RangeError, /graph too large \(n > 1024\)/, "floydWarshall refuses n > 1024");
});
__kitS.end();
const neg = graph(true, true, 2, [[0, 1, -1]]);
throws(() => neg.shortestPath(0, 1), RangeError, /shortestPath: negative edge weight/, "shortestPath with a negative weight");
throws(() => neg.shortestPathTree(0), RangeError, /shortestPathTree: negative edge weight/, "shortestPathTree with a negative weight");
throws(() => neg.shortestPath(2, 0), RangeError, /source out of range/, "shortestPath source out of range");
throws(() => neg.shortestPath(0, 2), RangeError, /destination out of range/, "shortestPath destination out of range");
throws(() => neg.shortestPathTree(5), RangeError, /source out of range/, "shortestPathTree source out of range");
throws(() => new Graph().criticalPath(), TypeError, /criticalPath requires a directed graph/, "criticalPath on an undirected graph");
throws(() => graph(true, false, 2, [[0, 1], [1, 0]]).criticalPath(), RangeError, /criticalPath: graph has a cycle/, "criticalPath on a cycle");
throws(() => graph(true, false, 1, [[0, 0]]).criticalPath(), RangeError, /criticalPath: graph has a cycle/, "a self loop is a cycle");
print(`test_structures_graph_paths_contract: ${pass} passed, ${fail} failed`);
if (fail) throw new Error(`${fail} contract failures`);
