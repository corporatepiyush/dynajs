// covers: dyna:graph.Graph
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_graph_graph_adversarial");
import { BitSet, UnionFind, Deque, Fenwick, RingBuffer, SegTree, BloomFilter, Trie, LRU, SortedSet, SortedMap, Heap, Multiset, Multimap, Table, RangeSet, RangeMap, IntervalTree, MinMaxHeap, CountMinSketch, HyperLogLog } from "dyna:structures";
import { Graph } from "dyna:graph";

let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { print("FAIL: " + m); fails++; } }
const J = (v) => JSON.stringify(v);
function throws(fn, kind, m) {
    n++;
    try { fn(); } catch (e) {
        if (kind && !(e instanceof kind)) { print("FAIL: " + m + " (wrong error: " + e + ")"); fails++; return; }
        return;
    }
    print("FAIL: " + m + " (no throw)"); fails++;
}

__kitS.section("aStar with a zero heuristic is exact", () => {
    const g = new Graph({ directed: true, weighted: true });
    for (let i = 0; i < 5; i++) g.addNode();
    g.addEdge(0, 1, 2).addEdge(1, 4, 3).addEdge(0, 2, 10).addEdge(2, 4, 1);
    g.addEdge(2, 3, 1).addEdge(3, 4, 1);
    check(g.aStar(0, 4, () => 0).dist === 5, "aStar with a zero heuristic is exact");

    throws(() => g.aStar(0, 4, (i) => { if (i === 1) g.addEdge(2, 3, 99); return 0; }),
           TypeError, "aStar rejects a heuristic that adds an edge");
    throws(() => g.aStar(0, 4, (i) => { if (i === 1) g.addNode(); return 0; }),
           TypeError, "aStar rejects a heuristic that adds a node");
    throws(() => g.aStar(0, 4, () => NaN), RangeError,
           "aStar rejects a NaN heuristic");
    throws(() => g.aStar(0, 4, () => Infinity), RangeError,
           "aStar rejects an infinite heuristic");
    const r = g.aStar(0, 4, (i) => i === 2 ? 100 : 0);
    check(Number.isFinite(r.dist), "aStar with an inadmissible heuristic terminates");
    check(g.edgeCount === 7 && g.nodeCount === 6,
          "the refused heuristics' mutations landed (edgeCount " + g.edgeCount +
          ", nodeCount " + g.nodeCount + ")");
    check(Number.isFinite(g.aStar(0, 4, () => 0).dist),
          "and aStar still runs on the mutated graph");
});

__kitS.section("connectedComponents on an empty graph is []", () => {
    const e = new Graph();
    const ed = new Graph({ directed: true });
    check(J(e.connectedComponents()) === J([]), "connectedComponents on an empty graph is []");
    check(e.nodeCount === 0 && e.edgeCount === 0, "an empty graph has no nodes or edges");
    throws(() => e.bfs(0), RangeError, "bfs on an empty graph throws");
    throws(() => e.dijkstra(0), RangeError, "dijkstra on an empty graph throws");
    throws(() => e.bellmanFord(0), RangeError, "bellmanFord on an empty graph throws");
    throws(() => e.aStar(0, 0, () => 0), RangeError, "aStar on an empty graph throws");
    check(J(e.floydWarshall()) === J([]), "floydWarshall on an empty graph is []");
    check(J(ed.topologicalSort()) === J([]),
          "topologicalSort on an empty directed graph is []");
    throws(() => e.topologicalSort(), TypeError,
           "topologicalSort on an undirected graph is refused");
    check(e.mst().weight === 0 && J(e.mst().edges) === J([]),
          "mst on an empty graph is weight 0 with no edges");

    const one = new Graph();
    one.addNode();
    check(one.mst().weight === 0, "mst of a single node has weight 0");
    check(J(one.connectedComponents()) === J([0]), "one node is one component");

    const neg = new Graph({ directed: true, weighted: true });
    neg.addNode();
    neg.addEdge(0, 0, -1);
    throws(() => neg.bellmanFord(0), RangeError,
           "bellmanFord detects a negative cycle via a self-loop");

    const fg = new Graph({ directed: true, weighted: true });
    fg.addEdge(0, 1, 1).addEdge(1, 0, -2);
    throws(() => fg.floydWarshall(), RangeError, "floydWarshall throws on a negative cycle");

    const ng = new Graph({ directed: true, weighted: true });
    ng.addEdge(0, 1, 2);
    ng.addEdge(2, 3, -5);
    throws(() => ng.dijkstra(0), RangeError, "dijkstra refuses an unreachable negative edge");
    throws(() => ng.aStar(0, 1, () => 0), RangeError, "aStar refuses an unreachable negative edge");

    const under = new Graph({ directed: true, weighted: true });
    under.addEdge(0, 1, -1e308).addEdge(1, 2, -1e308);
    const bf = under.bellmanFord(0);
    check(bf[2] === -Infinity,
          "bellmanFord reports the true -Infinity distance instead of a false cycle");
});
__kitS.end();

if (fails === 0) print("test_structures_adversarial: all " + n + " checks passed");
else print("test_structures_adversarial: " + fails + " FAILED of " + n);
