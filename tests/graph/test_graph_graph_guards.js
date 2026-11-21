// covers: dyna:graph.Graph
// Graph guards that no other suite reaches.
// 1. A visitor that mutates the graph: bfs, dfs and topologicalSort each throw a TypeError
//    naming their own op ("<op>: the visitor mutated the graph"), for a node added (n changes)
//    and for an edge added (only m changes). A swapped op name in the binding was caught by
//    the differential but by no suite until this file.
// 2. The aStar push budget (64n + 1024 pushes). The heuristic h(v) = -1e9 * v makes the
//    highest id pop first; on the complete DAG 0..k with w(i,j) = sum of 2^(k-l) over the
//    skipped ids i<l<j every subset path to k is a strict improvement found in worst order,
//    so the search pushes exactly 2^k times. The target is an isolated node so the search
//    never stops early. k = 10 (1024 pushes, budget 1792) completes; k = 12 (4096 pushes,
//    budget 1920) throws RangeError "aStar: heuristic reopens too many nodes (push budget
//    exceeded)".
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_graph_graph_guards");
import { Graph } from "dyna:graph";

let n = 0;
function assert(c, m) { n++; if (!c) throw new Error("assert: " + m); }
function throwsMsg(fn, type, msg, m) {
    let e = null;
    try { fn(); } catch (x) { e = x; }
    assert(e instanceof type && e.message === msg, m + " (got " + (e ? e.name + ": " + e.message : "no throw") + ")");
}

function chain() {
    const g = new Graph({ directed: true });
    g.addEdge(0, 1); g.addEdge(1, 2); g.addEdge(2, 3);
    return g;
}

for (const op of ["bfs", "dfs"]) {
    let g = chain();
    throwsMsg(() => g[op](0, { visit() { g.addNode(); } }), TypeError,
        op + ": the visitor mutated the graph", op + " visitor adds a node");
    g = chain();
    throwsMsg(() => g[op](0, { visit() { g.addEdge(3, 0); } }), TypeError,
        op + ": the visitor mutated the graph", op + " visitor adds an edge");
    assert(g[op](0).length === 4, op + " still walks after the throw");
    g = chain();
    let seen = 0;
    assert(g[op](0, { visit() { seen++; } }) === 4 && seen === 4, op + " a non-mutating visitor sees every node");
}

__kitS.section("topologicalSort: the visitor mutated the graph", () => {
    let g = chain();
    throwsMsg(() => g.topologicalSort({ visit() { g.addNode(); } }), TypeError,
        "topologicalSort: the visitor mutated the graph", "topologicalSort visitor adds a node");
    g = chain();
    throwsMsg(() => g.topologicalSort({ visit() { g.addEdge(0, 3); } }), TypeError,
        "topologicalSort: the visitor mutated the graph", "topologicalSort visitor adds an edge");
    assert(g.topologicalSort().length === 4, "topologicalSort still works after the throw");
});

function reopener(k) {
    const g = new Graph({ directed: true, weighted: true });
    for (let v = 0; v <= k + 1; v++) g.addNode();
    for (let i = 0; i <= k; i++)
        for (let j = i + 1; j <= k; j++) {
            let w = 0;
            for (let l = i + 1; l < j; l++) w += 2 ** (k - l);
            g.addEdge(i, j, w);
        }
    return g;
}

__kitS.section("k=10: 1024 pushes fit the budget, the isolated target is unreachable", () => {
    const k = 10, g = reopener(k);
    const r = g.aStar(0, k + 1, (v) => -1e9 * v);
    assert(r.dist === Infinity && r.path.length === 0, "k=10: 1024 pushes fit the budget, the isolated target is unreachable");
    assert(g.dijkstra(0, k) === 0 && g.dijkstra(0, k - 1) === 0, "k=10: the path through every id costs 0");
});

__kitS.section("aStar: heuristic reopens too many nodes (push budget exceeded)", () => {
    const k = 12, g = reopener(k);
    throwsMsg(() => g.aStar(0, k + 1, (v) => -1e9 * v), RangeError,
        "aStar: heuristic reopens too many nodes (push budget exceeded)", "k=12: 4096 pushes exceed 64n+1024");
    assert(g.aStar(0, k + 1, () => 0).dist === Infinity, "k=12: a zero heuristic stays within the budget");
});
__kitS.end();

print("graph guards: " + n + " passed");
