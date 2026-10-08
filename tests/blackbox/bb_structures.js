// Parametric black-box contract test for dyna:structures, generated from dynajs.d.ts lines 4910-5355. Engine sources not consulted.

import {
    Graph, LRU, Heap, MinMaxHeap, SortedSet, SortedMap, BTree, Deque, List,
    RingBuffer, BitSet, UnionFind, Fenwick, SegTree, BloomFilter, Trie,
    Multiset, Multimap, BiMap, Table, RangeSet, RangeMap, IntervalTree,
    CountMinSketch, HyperLogLog,
} from "dyna:structures";

let n = 0;
function assert(cond, msg) { n++; if (!cond) throw new Error("assertion failed: " + msg); }
function assertEq(actual, expected, msg) { n++; const ok = Object.is(actual, expected) || (typeof actual === "number" && typeof expected === "number" && Number.isNaN(actual) && Number.isNaN(expected)); if (!ok) throw new Error("assertion failed: " + msg + " — got |" + actual + "| expected |" + expected + "|"); }
function assertThrows(fn, msg, ErrType, errPattern) { n++; let threw = false, e = null; try { fn(); } catch (err) { threw = true; e = err; } if (!threw) throw new Error("expected throw: " + msg); if (ErrType && !(e instanceof ErrType)) throw new Error("wrong error type " + (e && e.constructor ? e.constructor.name : String(e)) + ": " + msg); if (errPattern && !(errPattern instanceof RegExp ? errPattern.test(String(e)) : String(e).includes(errPattern))) throw new Error("wrong error message |" + e + "|: " + msg); }
function eqArr(a, b) { if (a.length !== b.length) return false; for (let i = 0; i < a.length; i++) { const x = a[i], y = b[i]; if (Array.isArray(x) || Array.isArray(y)) { if (!eqArr(x, y)) return false; } else if (x !== y) return false; } return true; }
function assertEqArr(actual, expected, msg) { n++; const A = ArrayBuffer.isView(actual) ? Array.from(actual) : actual; if (!eqArr(A, expected)) throw new Error("assertion failed (arr): " + msg + " — got |" + JSON.stringify(A) + "| expected |" + JSON.stringify(expected) + "|"); }

const SKIP = Symbol("skip");

/* ---- ops-table machinery -----------------------------------------
 * A row is [label, ops, expected]: ops entries are [name, ...args] and
 * expected[i] is the return value of op i (SKIP to not pin). A getter
 * is named directly ("size"); the pseudo-op "$iter" spreads the
 * instance through [Symbol.iterator]. One loop drives every row and
 * failures name the row label.
 * ------------------------------------------------------------------ */
function normVal(x) {
    if (ArrayBuffer.isView(x)) return Array.from(x);
    if (Array.isArray(x)) return x.map(normVal);
    return x;
}
function jv(x) {
    const t = normVal(x);
    if (t === undefined) return "\u2039undefined\u203a";
    const s = JSON.stringify(t);
    return s === undefined ? "\u2039undefined\u203a" : s;
}
function cmpVal(got, exp, msg) {
    const g = jv(got), e = jv(exp);
    if (g !== e) throw new Error("assertion failed: " + msg + " — got |" + g + "| expected |" + e + "|");
    n++;
}
function runOps(inst, ops, expected, label) {
    for (let i = 0; i < ops.length; i++) {
        const op = ops[i];
        const name = op[0];
        const args = op.slice(1);
        let got;
        if (name === "$iter") got = [...inst];
        else if (typeof inst[name] === "function") got = inst[name](...args);
        else got = inst[name];
        if (expected[i] === SKIP) { n++; continue; } // SKIP pins nothing
        cmpVal(got, expected[i], label + " — op#" + i + " " + name);
    }
}
function runTable(rows, make) {
    for (const [label, ops, expected] of rows) {
        const inst = make();
        try { runOps(inst, ops, expected, label); } catch (e) { throw new Error("row [" + label + "]: " + (e && e.message ? e.message : String(e))); }
    }
}
/* Rows whose expected values are order-free sets: [label, fn]. */
function runBespoke(rows) {
    for (const [label, fn] of rows) {
        try { fn(); } catch (e) { throw new Error("row [" + label + "]: " + (e && e.message ? e.message : String(e))); }
    }
}
function sortedEqArr(got, expected, msg) { assertEqArr([...got].sort(), expected, msg); }

/* ================================================================== *
 *  Graph
 * ================================================================== */
runBespoke([
    ["addNode returns sequential ids 0,1,2", () => {
        const g = new Graph();
        assertEq(g.addNode(), 0, "first node id");
        assertEq(g.addNode(), 1, "second node id");
        assertEq(g.addNode(), 2, "third node id");
        assertEq(g.nodeCount, 3, "nodeCount");
    }],
    ["addEdge is chainable (returns this)", () => {
        const g = new Graph();
        assertEq(g.addEdge(0, 1), g, "addEdge returns this");
    }],
    ["undirected default: edge visible in both directions", () => {
        const g = new Graph();
        g.addEdge(0, 1);
        assertEq(g.hasEdge(0, 1), true, "u->v present");
        assertEq(g.hasEdge(1, 0), true, "v->u present (both-direction)");
    }],
    ["directed option: edge only u->v; edgeCount counts once", () => {
        const g = new Graph({ directed: true });
        g.addEdge(0, 1);
        assertEq(g.hasEdge(0, 1), true, "directed edge u->v");
        assertEq(g.hasEdge(1, 0), false, "no reverse edge");
        assertEq(g.edgeCount, 1, "edgeCount");
        assertEq(g.nodeCount, 2, "nodes grew on demand");
    }],
    ["nodes grow on demand from far ids", () => {
        const g = new Graph();
        g.addEdge(5, 7);
        assertEq(g.nodeCount, 8, "ids 0..7 materialized");
    }],
    ["neighbors and neighborsInto are the same door (parity)", () => {
        const g = new Graph();
        g.addEdge(0, 1); g.addEdge(0, 2); g.addEdge(0, 3);
        const nb = g.neighbors(0);
        const out = new Int32Array(3);
        const deg = g.neighborsInto(0, out);
        assertEq(deg, 3, "degree returned");
        assertEqArr(out, nb, "out content equals neighbors(0)");
        sortedEqArr(nb, [1, 2, 3], "targets of u's outgoing edges");
    }],
    ["neighborsInto: RangeError on a short out, NOTHING written", () => {
        const g = new Graph();
        g.addEdge(0, 1); g.addEdge(0, 2); g.addEdge(0, 3);
        const out = new Int32Array(2).fill(-1);
        assertThrows(() => g.neighborsInto(0, out), "short out refuses", RangeError);
        assertEqArr(out, [-1, -1], "nothing written on refusal");
    }],
    ["neighborsInto: extra capacity left untouched", () => {
        const g = new Graph();
        g.addEdge(0, 1); g.addEdge(0, 2); g.addEdge(0, 3);
        const out = new Int32Array(5).fill(-1);
        assertEq(g.neighborsInto(0, out), 3, "degree");
        assertEqArr(Array.from(out).slice(3), [-1, -1], "extra slots untouched");
    }],
    ["exportCSR is node-ordered and matches neighbors (parity)", () => {
        const g = new Graph();
        g.addEdge(0, 1); g.addEdge(0, 2); g.addEdge(1, 3); g.addEdge(2, 3);
        const { offsets, edges } = g.exportCSR();
        assertEq(offsets.length, g.nodeCount + 1, "offsets has n+1 entries");
        for (let u = 0; u < g.nodeCount; u++) {
            const slice = Array.from(edges.slice(offsets[u], offsets[u + 1])).sort((a, b) => a - b);
            assertEqArr(slice, g.neighbors(u).sort((a, b) => a - b), "CSR slice for node " + u);
        }
        assertEq(offsets[g.nodeCount], edges.length, "last offset covers all edges");
    }],
    ["bfs array form reaches the whole component", () => {
        const g = new Graph();
        g.addEdge(0, 1); g.addEdge(0, 2); g.addEdge(1, 3);
        sortedEqArr(g.bfs(0), [0, 1, 2, 3], "bfs(0) covers all (order not pinned)");
    }],
    ["bfs visitor: counts, early stop counts the stopping visit", () => {
        const g = new Graph();
        g.addEdge(0, 1); g.addEdge(0, 2); g.addEdge(1, 3);
        let calls = 0;
        const all = g.bfs(0, { visit: () => { calls++; return false; } });
        assertEq(all, 4, "visited count for a falsy visitor");
        assertEq(calls, 4, "visit called once per node");
        const early = g.bfs(0, { visit: () => true });
        assertEq(early, 1, "early stop returns 1, counting the stopping node");
    }],
    ["dfs visitor visits smaller ids first (order pinned by doc)", () => {
        const g = new Graph();
        g.addEdge(0, 1); g.addEdge(0, 2); g.addEdge(1, 3);
        const order = [];
        const n0 = g.dfs(0, { visit: (node) => { order.push(node); } });
        assertEqArr(order, [0, 1, 3, 2], "dfs order, smaller ids first");
        assertEq(n0, 4, "visited count");
        const stop = g.dfs(0, { visit: (node) => node === 3 });
        assertEq(stop, 3, "early stop counts [0,1,3]");
    }],
    ["dijkstra distances (directed, weighted), single-dst overload", () => {
        // "weighted (boolean, default false) — edges carry weights" (dynajs.d.ts);
        // d.ts opts bag: weights only count when the graph is weighted.
        const g = new Graph({ directed: true, weighted: true });
        g.addEdge(0, 1, 1); g.addEdge(1, 2, 1); g.addEdge(0, 2, 5);
        assertEqArr(g.dijkstra(0), [0, 1, 2], "distances via 1+1 < 5");
        assertEq(g.dijkstra(0, 2), 2, "single distance to dst");
    }],
    ["bellmanFord parity with dijkstra", () => {
        // weights only count on a weighted graph (dynajs.d.ts: unweighted forces w=1.0)
        const g = new Graph({ directed: true, weighted: true });
        g.addEdge(0, 1, 1); g.addEdge(1, 2, 1); g.addEdge(0, 2, 5);
        assertEqArr(g.bellmanFord(0), [0, 1, 2], "same distances as dijkstra");
    }],
    ["topologicalSort unique on a chain; visitor + early exit", () => {
        const g = new Graph({ directed: true });
        g.addEdge(0, 1); g.addEdge(1, 2);
        assertEqArr(g.topologicalSort(), [0, 1, 2], "chain order unique");
        let seen = 0;
        assertEq(g.topologicalSort({ visit: () => { seen++; return false; } }), 3, "visitor form returns visited count");
        assertEq(g.topologicalSort({ visit: () => true }), 1, "early exit after first visit");
    }],
    ["topologicalSort throws RangeError on a cycle", () => {
        const g = new Graph({ directed: true });
        g.addEdge(0, 1); g.addEdge(1, 0);
        assertThrows(() => g.topologicalSort(), "cycle refuses", RangeError);
    }],
    ["connectedComponents: relational ids (assignment not pinned)", () => {
        const g = new Graph();
        g.addEdge(0, 1); g.addEdge(2, 3); g.addNode();
        const cc = g.connectedComponents();
        assertEq(cc.length, 5, "one entry per node");
        assertEq(cc[0] === cc[1], true, "0,1 together");
        assertEq(cc[2] === cc[3], true, "2,3 together");
        assertEq(cc[0] !== cc[2] && cc[0] !== cc[4] && cc[2] !== cc[4], true, "the three components are distinct");
    }],
    ["floydWarshall all-pairs on a weighted chain", () => {
        // weights only count on a weighted graph (dynajs.d.ts: unweighted forces w=1.0)
        const g = new Graph({ weighted: true });
        g.addEdge(0, 1, 1); g.addEdge(1, 2, 2);
        const m = g.floydWarshall();
        assertEqArr(m[0], [0, 1, 3], "row 0");
        assertEqArr(m[1], [1, 0, 2], "row 1");
        assertEqArr(m[2], [3, 2, 0], "row 2");
    }],
    ["mst: minimum spanning forest of a weighted triangle", () => {
        // weights only count on a weighted graph (dynajs.d.ts: unweighted forces w=1.0)
        const g = new Graph({ weighted: true });
        g.addEdge(0, 1, 1); g.addEdge(1, 2, 2); g.addEdge(0, 2, 3);
        const t = g.mst();
        assertEq(t.weight, 3, "weight 1+2 (3-edge refused)");
        assertEq(t.edges.length, 2, "n-1 edges");
        const has = (u, v) => t.edges.some((e) => (e[0] === u && e[1] === v) || (e[0] === v && e[1] === u));
        assert(has(0, 1) && has(1, 2), "picked edges {0-1} and {1-2}");
    }],
    ["aStar with a zero heuristic = dijkstra path", () => {
        const g = new Graph({ directed: true });
        g.addEdge(0, 1, 1); g.addEdge(1, 2, 1);
        const r = g.aStar(0, 2, () => 0);
        assertEq(r.dist, 2, "distance");
        assertEqArr(r.path, [0, 1, 2], "unique path on a chain");
    }],
]);

/* ================================================================== *
 *  LRU — capacity-bounded string cache with LRU eviction.
 * ================================================================== */
runTable([
    ["put/get/has basics", [["put", "a", 1], ["get", "a"], ["has", "a"], ["get", "zz"], ["has", "zz"]], [SKIP, 1, true, undefined, false]],
    ["size counts distinct keys; put overwrites in place", [["put", "a", 1], ["put", "b", 2], ["size"], ["put", "a", 9], ["size"], ["get", "a"]], [SKIP, SKIP, 2, SKIP, 2, 9]],
    ["LRU eviction drops the least recently USED entry", [["put", "a", 1], ["put", "b", 2], ["put", "c", 3], ["has", "a"], ["has", "b"], ["has", "c"], ["size"]], [SKIP, SKIP, SKIP, false, true, true, 2]],
    ["get refreshes recency (b becomes the LRU victim)", [["put", "a", 1], ["put", "b", 2], ["get", "a"], ["put", "c", 3], ["has", "b"], ["has", "a"], ["has", "c"]], [SKIP, SKIP, 1, SKIP, false, true, true]],
    ["delete returns true once then false", [["put", "a", 1], ["delete", "a"], ["delete", "a"], ["has", "a"]], [SKIP, true, false, false]],
], () => new LRU(2));

runTable([
    ["capacity boundary 1: new put evicts", [["put", "a", 1], ["put", "b", 2], ["has", "a"], ["get", "b"], ["capacity"], ["size"]], [SKIP, SKIP, false, 2, 1, 1]],
], () => new LRU(1));

runBespoke([
    ["stats counters hand-computed (hits/misses/evictions)", () => {
        const l = new LRU(2);
        l.put("a", 1);
        l.get("a");              // hit
        l.get("x");              // miss
        l.put("b", 2);
        l.put("c", 3);           // evicts a
        const s = l.stats;
        assertEq(s.hits, 1, "hits");
        assertEq(s.misses, 1, "misses");
        assertEq(s.evictions, 1, "evictions");
        assertEq(s.size, 2, "stats size");
        assertEq(s.capacity, 2, "stats capacity");
        assertEq(l.capacity, 2, "capacity property");
    }],
    ["onEvict fires with the evicted key and value", () => {
        const evicted = [];
        const l = new LRU(1, { onEvict: (k, v) => evicted.push([k, v]) });
        l.put("a", 1);
        l.put("b", 2);
        assertEqArr(evicted.length !== 1 ? [] : evicted[0], ["a", 1], "eviction callback payload");
    }],
    ["setWithTTL + purgeExpired reclaims expired entries (60ms sleep)", async () => {
        const l = new LRU(2);
        l.setWithTTL("t", 5, 25);
        await sleep(60);
        assertEq(l.purgeExpired(), 1, "purgeExpired returns the count removed");
        assertEq(l.size, 0, "cache empty after purge");
        assertEq(l.has("t"), false, "expired key gone");
    }],
    ["put and set are two doors of one door (parity)", () => {
        const a = new LRU(2);
        const b = new LRU(2);
        a.put("k", 7);
        b.set("k", 7);
        assertEq(b.get("k"), a.get("k"), "same value through both doors");
        assertEq(b.size, a.size, "same size");
    }],
    ["constructor rejects unknown option keys with a TypeError naming the key", () => {
        assertThrows(() => new LRU(2, { bogus: 1 }), "strict options bag", TypeError, /bogus/);
    }],
]);

/* ================================================================== *
 *  Heap — natural order and comparator doors.
 * ================================================================== */
runTable([
    ["natural min-order: push returns the NEW size, pops ascend", [["push", 3], ["push", 1], ["push", 2], ["pop"], ["pop"], ["pop"], ["pop"], ["size"]], [1, 2, 3, 1, 2, 3, undefined, 0]],
    ["peek returns the min without removing", [["push", 5], ["peek"], ["size"]], [1, 5, 1]],
    ["empty heap pops/peeks undefined", [["pop"], ["peek"]], [undefined, undefined]],
    // push "returns the new size" (d.ts); length mirrors size, so both read 2 after two pushes
    ["length mirrors size", [["push", 7], ["push", 8], ["length"], ["size"]], [1, 2, 2, 2]],
], () => new Heap());

runTable([
    ["descending comparator: pops descend", [["push", 1], ["push", 2], ["push", 3], ["pop"], ["pop"], ["pop"]], [1, 2, 3, 3, 2, 1]],
], () => new Heap((a, b) => b - a));

runBespoke([
    ["Heap round trip: explicit-cmp and natural deserialize doors agree", () => {
        const h = new Heap();
        h.push(3); h.push(1); h.push(2);
        const bytes = h.serialize();
        const viaCmp = Heap.deserialize(bytes, (a, b) => a - b);
        assertEq(viaCmp.pop(), 1, "cmp door pops min");
        const natural = Heap.deserialize(bytes);
        assertEq(natural.pop(), 1, "natural door pops min (parity)");
    }],
]);

/* ================================================================== *
 *  MinMaxHeap — both ends peek/pop.
 * ================================================================== */
runTable([
    ["peekMin/peekMax on (1,a),(3,c),(2,b)", [["push", 1, "a"], ["push", 3, "c"], ["push", 2, "b"], ["peekMin"], ["peekMax"], ["size"]], [SKIP, SKIP, SKIP, "a", "c", 3]],
    ["popMin then popMax drain both ends", [["push", 1, "a"], ["push", 3, "c"], ["push", 2, "b"], ["popMin"], ["popMax"], ["popMin"], ["size"], ["popMin"]], [SKIP, SKIP, SKIP, "a", "c", "b", 0, undefined]],
    ["empty: both ends undefined", [["peekMin"], ["popMax"]], [undefined, undefined]],
], () => new MinMaxHeap());

/* ================================================================== *
 *  SortedSet — numbers in sorted order.
 * ================================================================== */
runTable([
    ["toArray is ascending regardless of insert order", [["add", 5], ["add", 1], ["add", 3], ["toArray"]], [SKIP, SKIP, SKIP, [1, 3, 5]]],
    ["adding a duplicate keeps one entry", [["add", 1], ["add", 1], ["size"]], [SKIP, SKIP, 1]],
    ["has membership", [["add", 1], ["has", 1], ["has", 2]], [SKIP, true, false]],
    ["delete returns true once then false", [["add", 1], ["delete", 1], ["delete", 1], ["has", 1], ["size"]], [SKIP, true, false, false, 0]],
    ["first/last on empty are undefined", [["first"], ["last"]], [undefined, undefined]],
    ["first/last on {1,3,5}", [["add", 5], ["add", 1], ["add", 3], ["first"], ["last"]], [SKIP, SKIP, SKIP, 1, 5]],
    ["floor: greatest <= x", [["add", 1], ["add", 3], ["add", 5], ["floor", 5], ["floor", 4], ["floor", 1], ["floor", 0]], [SKIP, SKIP, SKIP, 5, 3, 1, undefined]],
    ["ceil: least >= x", [["add", 1], ["add", 3], ["add", 5], ["ceil", 1], ["ceil", 2], ["ceil", 5], ["ceil", 6]], [SKIP, SKIP, SKIP, 1, 3, 5, undefined]],
    ["rangeQuery [lo,hi] inclusive at both ends", [["add", 1], ["add", 3], ["add", 5], ["rangeQuery", 1, 5], ["rangeQuery", 2, 5], ["rangeQuery", 3, 3], ["rangeQuery", 4, 4]], [SKIP, SKIP, SKIP, [1, 3, 5], [3, 5], [3], []]],
    ["iteration is ascending", [["add", 5], ["add", 1], ["add", 3], ["$iter"]], [SKIP, SKIP, SKIP, [1, 3, 5]]],
], () => new SortedSet());

/* ================================================================== *
 *  SortedMap vs BTree — same surface, DISTINCT serialize formats.
 * ================================================================== */
const SM_ROWS = [
    ["set/get/has", [["set", 2, "b"], ["set", 1, "a"], ["get", 1], ["get", 2], ["get", 3], ["has", 2]], [SKIP, SKIP, "a", "b", undefined, true]],
    ["keys ascending", [["set", 3, "v3"], ["set", 1, "v1"], ["set", 2, "v2"], ["keys"]], [SKIP, SKIP, SKIP, [1, 2, 3]]],
    ["delete returns true once then false", [["set", 1, "v1"], ["delete", 1], ["delete", 1], ["size"]], [SKIP, true, false, 0]],
    ["firstKey/lastKey on empty are undefined", [["firstKey"], ["lastKey"]], [undefined, undefined]],
    ["firstKey/lastKey", [["set", 5, "v5"], ["set", 1, "v1"], ["set", 3, "v3"], ["firstKey"], ["lastKey"]], [SKIP, SKIP, SKIP, 1, 5]],
    ["floorKey/ceilKey exact and between", [["set", 1, "v1"], ["set", 3, "v3"], ["set", 5, "v5"], ["floorKey", 4], ["ceilKey", 4], ["floorKey", 0], ["ceilKey", 6], ["floorKey", 3], ["ceilKey", 3]], [SKIP, SKIP, SKIP, 3, 5, undefined, undefined, 3, 3]],
    ["rangeQuery [lo,hi] inclusive tuples", [["set", 1, "v1"], ["set", 3, "v3"], ["set", 5, "v5"], ["rangeQuery", 2, 4], ["rangeQuery", 1, 5], ["rangeQuery", 6, 7]], [SKIP, SKIP, SKIP, [[3, "v3"]], [[1, "v1"], [3, "v3"], [5, "v5"]], []]],
];
runTable(SM_ROWS, () => new SortedMap());
runTable(SM_ROWS, () => new BTree());

runTable([
    ["BTree is the iterable one: [k,v] ascending", [["set", 2, "v2"], ["set", 1, "v1"], ["set", 3, "v3"], ["$iter"]], [SKIP, SKIP, SKIP, [[1, "v1"], [2, "v2"], [3, "v3"]]]],
], () => new BTree());

runBespoke([
    ["SortedMap has NO [Symbol.iterator] (doc: 'SortedMap has no [Symbol.iterator]')", () => {
        const sm = new SortedMap();
        assertEq(sm[Symbol.iterator], undefined, "no iterator on SortedMap");
    }],
    ["BTree vs SortedMap serialize formats are DISTINCT: cross-deserialize refuses both ways", () => {
        const bt = new BTree();
        bt.set(1, "v1");
        const sm = new SortedMap();
        sm.set(1, "v1");
        assertThrows(() => SortedMap.deserialize(bt.serialize()), "SortedMap refuses a BTree record");
        assertThrows(() => BTree.deserialize(sm.serialize()), "BTree refuses a SortedMap record");
    }],
    ["SortedMap round trip", () => {
        const sm = new SortedMap();
        sm.set(2, "b"); sm.set(1, "a");
        const back = SortedMap.deserialize(sm.serialize());
        assertEqArr(back.keys(), [1, 2], "keys parity");
        assertEq(back.get(2), "b", "value parity");
    }],
    ["BTree round trip", () => {
        const bt = new BTree();
        bt.set(2, "b"); bt.set(1, "a");
        const back = BTree.deserialize(bt.serialize());
        assertEqArr(back.keys(), [1, 2], "keys parity");
        assertEq([...back][0][1], "a", "iterator parity");
    }],
]);

/* ================================================================== *
 *  Deque / List / RingBuffer
 * ================================================================== */
runTable([
    ["pushBack/pushFront order", [["pushBack", 1], ["pushBack", 2], ["pushFront", 0], ["toArray"]], [SKIP, SKIP, SKIP, [0, 1, 2]]],
    ["popFront drains FIFO, undefined at empty", [["pushBack", 1], ["pushBack", 2], ["popFront"], ["popFront"], ["popFront"]], [SKIP, SKIP, 1, 2, undefined]],
    ["popBack drains LIFO", [["pushBack", 1], ["pushBack", 2], ["popBack"], ["popBack"]], [SKIP, SKIP, 2, 1]],
    ["peeks on empty are undefined", [["peekFront"], ["peekBack"]], [undefined, undefined]],
    ["peeks do not remove", [["pushBack", 1], ["pushBack", 2], ["peekFront"], ["peekBack"], ["length"]], [SKIP, SKIP, 1, 2, 2]],
    ["get positional; out-of-range undefined", [["pushBack", 9], ["pushBack", 8], ["get", 0], ["get", 1], ["get", 5]], [SKIP, SKIP, 9, 8, undefined]],
    ["iteration front-to-back", [["pushBack", 1], ["pushFront", 0], ["$iter"]], [SKIP, SKIP, [0, 1]]],
], () => new Deque());

runBespoke([
    ["Deque round trip", () => {
        const d = new Deque();
        d.pushBack(1); d.pushFront(0);
        const back = Deque.deserialize(d.serialize());
        assertEqArr(back.toArray(), [0, 1], "toArray parity");
    }],
    ["List element identity is stable", () => {
        const l = new List();
        const obj = { id: 42 };
        l.pushBack(obj);
        assertEq(l.popBack(), obj, "popped value is the SAME reference");
    }],
    ["List round trip", () => {
        const l = new List();
        l.pushBack(1); l.pushFront(0); l.pushBack(2);
        const back = List.deserialize(l.serialize());
        assertEqArr(back.toArray(), [0, 1, 2], "toArray parity");
    }],
]);

runTable([
    ["List order via toArray", [["pushBack", 1], ["pushBack", 2], ["pushFront", 0], ["toArray"], ["length"]], [SKIP, SKIP, SKIP, [0, 1, 2], 3]],
    ["List popFront/popBack and empties", [["pushBack", 1], ["popFront"], ["popBack"], ["front"], ["back"]], [SKIP, 1, undefined, undefined, undefined]],
    ["List front/back peek both ends", [["pushBack", 1], ["pushBack", 2], ["front"], ["back"]], [SKIP, SKIP, 1, 2]],
    ["List iteration", [["pushBack", 1], ["pushFront", 0], ["$iter"]], [SKIP, SKIP, [0, 1]]],
], () => new List());

runTable([
    ["fills to capacity and reports full", [["push", 1], ["push", 2], ["push", 3], ["length"], ["full"]], [SKIP, SKIP, SKIP, 3, true]],
    ["push over capacity overwrites the OLDEST", [["push", 1], ["push", 2], ["push", 3], ["push", 4], ["toArray"], ["full"]], [SKIP, SKIP, SKIP, SKIP, [2, 3, 4], true]],
    ["get after wrap; out-of-range undefined", [["push", 1], ["push", 2], ["push", 3], ["push", 4], ["get", 0], ["get", 2], ["get", 3]], [SKIP, SKIP, SKIP, SKIP, 2, 4, undefined]],
    ["capacity property", [["capacity"]], [3]],
    ["iteration after wrap", [["push", 1], ["push", 2], ["push", 3], ["push", 4], ["$iter"]], [SKIP, SKIP, SKIP, SKIP, [2, 3, 4]]],
], () => new RingBuffer(3));

runTable([
    // own instance: the wrapping row needs capacity 2 (shared make() above pins 3)
    ["wrap at capacity 2", [["push", "a"], ["push", "b"], ["push", "c"], ["toArray"]], [SKIP, SKIP, SKIP, ["b", "c"]]],
], () => new RingBuffer(2));

runBespoke([
    ["RingBuffer round trip keeps capacity and window", () => {
        const rb = new RingBuffer(3);
        rb.push(1); rb.push(2); rb.push(3); rb.push(4);
        const back = RingBuffer.deserialize(rb.serialize());
        assertEq(back.capacity, 3, "capacity parity");
        assertEqArr(back.toArray(), [2, 3, 4], "window parity");
    }],
]);

/* ================================================================== *
 *  BitSet
 * ================================================================== */
runTable([
    ["set/get/count", [["set", 5], ["get", 5], ["get", 6], ["count"]], [SKIP, true, false, 1]],
    ["dynamic growth past the constructor hint", [["set", 70], ["get", 70], ["count"]], [SKIP, true, 1]],
    ["clear resets a bit", [["set", 5], ["clear", 5], ["get", 5], ["count"]], [SKIP, SKIP, false, 0]],
    ["flip toggles", [["set", 5], ["flip", 5], ["get", 5], ["count"], ["flip", 5], ["get", 5], ["count"]], [SKIP, SKIP, false, 0, SKIP, true, 1]],
    ["nextSet: first set bit >= from, else -1", [["set", 3], ["set", 10], ["nextSet", 0], ["nextSet", 3], ["nextSet", 4], ["nextSet", 11]], [SKIP, SKIP, 3, 3, 10, -1]],
    ["toArray and iteration ascend across words", [["set", 65], ["set", 0], ["toArray"], ["$iter"]], [SKIP, SKIP, [0, 65], [0, 65]]],
], () => new BitSet(64));

runBespoke([
    ["and/or/xor on fresh sets", () => {
        const mk = (bits) => { const b = new BitSet(64); for (const i of bits) b.set(i); return b; };
        const other = mk([2, 3]);
        assertEqArr(mk([1, 2]).and(other).toArray(), [2], "and");
        assertEqArr(mk([1, 2]).or(other).toArray(), [1, 2, 3], "or");
        assertEqArr(mk([1, 2]).xor(other).toArray(), [1, 3], "xor");
    }],
    ["BitSet round trip across words", () => {
        const b = new BitSet(64);
        b.set(5); b.set(70);
        const back = BitSet.deserialize(b.serialize());
        assertEq(back.count, 2, "count parity");
        assertEq(back.get(70), true, "high bit parity");
    }],
]);

/* ================================================================== *
 *  UnionFind
 * ================================================================== */
runTable([
    ["fresh forest sizes", [["count"], ["size"]], [5, 5]],
    ["union merges once; count drops", [["union", 0, 1], ["union", 1, 2], ["union", 0, 2], ["connected", 0, 2], ["connected", 0, 3], ["count"]], [true, true, false, true, false, 3]],
    ["an element is connected to itself", [["connected", 3, 3]], [true]],
], () => new UnionFind(5));

runBespoke([
    ["find parity: same component, same root; distinct components differ", () => {
        const uf = new UnionFind(5);
        uf.union(0, 1); uf.union(1, 2);
        assertEq(uf.find(0), uf.find(2), "same root in one component");
        assertEq(uf.find(0) !== uf.find(3), true, "different roots across components");
    }],
    ["UnionFind round trip", () => {
        const uf = new UnionFind(5);
        uf.union(0, 1);
        const back = UnionFind.deserialize(uf.serialize());
        assertEq(back.connected(0, 1), true, "connection parity");
        assertEq(back.connected(2, 3), false, "non-connection parity");
        assertEq(back.count, 4, "component count parity");
    }],
]);

/* ================================================================== *
 *  Fenwick — prefix sums over doubles.
 * ================================================================== */
runTable([
    ["prefix and range sums hand-computed", [["update", 0, 1], ["update", 2, 3], ["update", 4, 2], ["prefixSum", 1], ["prefixSum", 2], ["prefixSum", 4], ["rangeQuery", 1, 3], ["rangeQuery", 2, 1], ["rangeQuery", 0, 4], ["size"]], [SKIP, SKIP, SKIP, 1, 4, 6, 3, 0, 6, 5]],
    ["negative deltas", [["update", 0, 1], ["update", 0, -5], ["prefixSum", 0]], [SKIP, SKIP, -4]],
], () => new Fenwick(5));

runTable([
    // ranges INCLUSIVE (module-consistent with Fenwick's [lo..hi]; see DOC-TENSION below):
    // [0..3]=12, [0..1]=5, [1..2]=0+7=7, [2..2]=7, [3..3]=0
    ["SegTree sum op", [["update", 0, 5], ["update", 2, 7], ["rangeQuery", 0, 3], ["rangeQuery", 0, 1], ["rangeQuery", 1, 2], ["rangeQuery", 2, 2], ["rangeQuery", 3, 3], ["size"]], [SKIP, SKIP, 12, 5, 7, 7, 0, 4]],
], () => new SegTree(4, "sum"));
// DOC-TENSION: SegTree ranges treated as INCLUSIVE — Fenwick documents [lo..hi]
// inclusive and SegTree is silent; the module-consistent reading is used.

runTable([
    ["SegTree min op", [["update", 0, 4], ["update", 1, 2], ["update", 2, 9], ["rangeQuery", 0, 2], ["rangeQuery", 0, 0], ["rangeQuery", 2, 2]], [SKIP, SKIP, SKIP, 2, 4, 9]],
], () => new SegTree(3, "min"));
runTable([
    ["SegTree max op", [["update", 0, 4], ["update", 1, 2], ["update", 2, 9], ["rangeQuery", 0, 2], ["rangeQuery", 0, 1], ["rangeQuery", 1, 2]], [SKIP, SKIP, SKIP, 9, 4, 9]],
], () => new SegTree(3, "max"));

runBespoke([
    ["Fenwick round trip", () => {
        const f = new Fenwick(5);
        f.update(0, 1); f.update(2, 3); f.update(4, 2);
        assertEq(Fenwick.deserialize(f.serialize()).prefixSum(4), 6, "prefixSum parity");
    }],
    ["SegTree round trip", () => {
        const s = new SegTree(4, "sum");
        s.update(0, 5); s.update(2, 7);
        assertEq(SegTree.deserialize(s.serialize()).rangeQuery(0, 3), 12, "rangeQuery parity");
    }],
]);

/* ================================================================== *
 *  BloomFilter — no false negatives.
 * ================================================================== */
runTable([
    ["configured geometry is exposed", [["bits"], ["hashes"]], [64, 3]],
    ["added key is always contained", [["add", "a"], ["mayContain", "a"]], [SKIP, true]],
], () => new BloomFilter(64, 3));

runBespoke([
    ["no false negatives over five keys", () => {
        const bf = new BloomFilter(64, 3);
        const keys = ["k1", "k2", "k3", "k4", "k5"];
        for (const k of keys) bf.add(k);
        for (const k of keys) assertEq(bf.mayContain(k), true, "added key " + k + " must be contained");
    }],
    ["BloomFilter round trip preserves membership", () => {
        const bf = new BloomFilter(64, 3);
        bf.add("alpha"); bf.add("beta");
        const back = BloomFilter.deserialize(bf.serialize());
        assertEq(back.mayContain("alpha"), true, "alpha parity");
        assertEq(back.mayContain("beta"), true, "beta parity");
    }],
]);

/* ================================================================== *
 *  Trie — string keys with prefix queries.
 * ================================================================== */
runTable([
    ["insert/has: a prefix is not a key", [["insert", "car"], ["insert", "cat"], ["insert", "cart"], ["has", "car"], ["has", "ca"], ["has", "cab"], ["size"]], [SKIP, SKIP, SKIP, true, false, false, 3]],
    ["delete removes exactly the key", [["insert", "car"], ["insert", "cat"], ["insert", "cart"], ["delete", "cat"], ["delete", "cat"], ["has", "cat"], ["has", "cart"], ["size"]], [SKIP, SKIP, SKIP, true, false, false, true, 2]],
    ["longestPrefix picks the longest stored prefix", [["insert", "car"], ["insert", "cat"], ["insert", "cart"], ["insert", "dog"], ["longestPrefix", "cartoon"], ["longestPrefix", "dogma"], ["longestPrefix", "car"], ["longestPrefix", "ca"], ["longestPrefix", "x"]], [SKIP, SKIP, SKIP, SKIP, "cart", "dog", "car", "", ""]],
], () => new Trie());

runBespoke([
    ["keysWithPrefix is a set of stored keys (order not pinned)", () => {
        const t = new Trie();
        t.insert("car"); t.insert("cat"); t.insert("cart"); t.insert("dog");
        sortedEqArr(t.keysWithPrefix("ca"), ["car", "cart", "cat"], "prefix ca");
        // sortedEqArr uses default .sort(); expectation must be in that (lexicographic) order
        sortedEqArr(t.keysWithPrefix(""), ["car", "cart", "cat", "dog"], "empty prefix = all");
        assertEq(t.keysWithPrefix("x").length, 0, "no matches");
        sortedEqArr([...t], ["car", "cart", "cat", "dog"], "iteration covers all keys (order not pinned)");
    }],
    ["Trie round trip", () => {
        const t = new Trie();
        t.insert("car"); t.insert("cart");
        const back = Trie.deserialize(t.serialize());
        assertEq(back.has("car"), true, "has parity");
        assertEq(back.longestPrefix("cartoon"), "cart", "longestPrefix parity");
    }],
]);

/* ================================================================== *
 *  Multiset — string key to uint64 count.
 * ================================================================== */
runTable([
    ["add defaults to +1 and returns the running count", [["add", "a"], ["add", "a"], ["count", "a"], ["count", "b"], ["has", "a"], ["has", "b"]], [1, 2, 2, 0, true, false]],
    ["add with explicit n", [["add", "a", 5], ["count", "a"]], [5, 5]],
    ["remove returns the post-removal count, floored at 0", [["add", "a"], ["add", "a"], ["remove", "a"], ["remove", "a"], ["count", "a"], ["has", "a"]], [SKIP, SKIP, 1, 0, 0, false]],
    ["setCount then read back", [["setCount", "c", 5], ["count", "c"], ["size"], ["totalSize"]], [SKIP, 5, 1, 5]],
    ["size = distinct keys, totalSize = sum of counts", [["add", "a"], ["add", "a"], ["add", "c", 5], ["size"], ["totalSize"], ["delete", "a"], ["delete", "zz"], ["size"], ["totalSize"]], [SKIP, SKIP, SKIP, 2, 7, true, false, 1, 5]],
    ["clear empties everything", [["add", "a", 2], ["clear"], ["size"], ["totalSize"]], [SKIP, undefined, 0, 0]],
], () => new Multiset());

runBespoke([
    ["elementSet/entrySet cover every distinct key (order not pinned)", () => {
        const m = new Multiset();
        m.add("a", 2); m.add("c", 5);
        sortedEqArr(m.elementSet(), ["a", "c"], "elementSet");
        const es = m.entrySet().slice().sort((x, y) => (x[0] < y[0] ? -1 : 1));
        assertEqArr(es, [["a", 2], ["c", 5]], "entrySet");
        const it = [...m].slice().sort((x, y) => (x[0] < y[0] ? -1 : 1));
        assertEqArr(it, [["a", 2], ["c", 5]], "iterator pairs");
    }],
    ["Multiset round trip", () => {
        const m = new Multiset();
        m.add("a", 2); m.add("c", 5);
        const back = Multiset.deserialize(m.serialize());
        assertEq(back.count("a"), 2, "count parity");
        assertEq(back.totalSize, 7, "totalSize parity");
    }],
]);

/* ================================================================== *
 *  Multimap — string key to growing value array.
 * ================================================================== */
runTable([
    ["put grows the per-key array; sizes tracked", [["put", "a", 1], ["put", "a", 2], ["put", "b", 3], ["get", "a"], ["count", "a"], ["count", "c"], ["size"], ["keyCount"]], [SKIP, SKIP, SKIP, [1, 2], 2, 0, 3, 2]],
    ["removeAt returns the value by index", [["put", "a", 1], ["put", "a", 2], ["removeAt", "a", 0], ["get", "a"], ["removeAt", "a", 9]], [SKIP, SKIP, 1, [2], undefined]],
    ["delete removes every value for the key and returns how many", [["put", "a", 1], ["put", "a", 2], ["put", "b", 3], ["delete", "a"], ["get", "a"], ["size"], ["keyCount"]], [SKIP, SKIP, SKIP, 2, [], 1, 1]],
], () => new Multimap());

runBespoke([
    ["keys/entries/iterator cover everything (order not pinned)", () => {
        const m = new Multimap();
        m.put("a", 1); m.put("b", 3);
        sortedEqArr(m.keys(), ["a", "b"], "keys");
        const es = m.entries().slice().sort((x, y) => (x[0] < y[0] ? -1 : 1));
        assertEqArr(es, [["a", 1], ["b", 3]], "entries");
        const it = [...m].slice().sort((x, y) => (x[0] < y[0] ? -1 : 1));
        assertEqArr(it, [["a", 1], ["b", 3]], "iterator pairs");
    }],
    ["Multimap round trip", () => {
        const m = new Multimap();
        m.put("a", 1); m.put("a", 2);
        assertEqArr(Multimap.deserialize(m.serialize()).get("a"), [1, 2], "get parity");
    }],
]);

/* ================================================================== *
 *  BiMap — two-way string map.
 * ================================================================== */
runTable([
    ["forward and inverse lookups", [["set", "a", "1"], ["set", "b", "2"], ["get", "a"], ["keyOf", "1"], ["has", "a"], ["has", "z"], ["hasValue", "2"], ["hasValue", "9"], ["size"]], [SKIP, SKIP, "1", "a", true, false, true, false, 2]],
    ["missing lookups are undefined", [["get", "x"], ["keyOf", "9"]], [undefined, undefined]],
    ["delete clears both directions", [["set", "a", "1"], ["delete", "a"], ["delete", "a"], ["has", "a"], ["hasValue", "1"], ["size"]], [SKIP, true, false, false, false, 0]],
    ["deleteValue clears both directions", [["set", "a", "1"], ["set", "b", "2"], ["deleteValue", "2"], ["get", "b"], ["hasValue", "2"], ["size"]], [SKIP, SKIP, true, undefined, false, 1]],
    ["forceSet replaces in place", [["set", "a", "1"], ["forceSet", "a", "9"], ["get", "a"], ["keyOf", "9"]], [SKIP, SKIP, "9", "a"]],
    ["overwriting a key cleans its old inverse (two-way reading)", [["set", "a", "1"], ["set", "a", "2"], ["keyOf", "1"], ["get", "a"]], [SKIP, SKIP, undefined, "2"]],
    ["clear empties", [["set", "a", "1"], ["clear"], ["size"]], [SKIP, undefined, 0]],
], () => new BiMap());
// DOC-TENSION: the last two rows assume bidirectional exclusivity (an old
// value's inverse is dropped when its key is rebound) — the natural reading
// of "two-way string-to-string map"; the doc does not spell out conflicts.

runBespoke([
    ["entries/inverseEntries/iterator are mirror images (order not pinned)", () => {
        const b = new BiMap();
        b.set("a", "1"); b.set("b", "2");
        const sort = (xs) => xs.slice().sort((x, y) => (x[0] < y[0] ? -1 : 1));
        assertEqArr(sort(b.entries()), [["a", "1"], ["b", "2"]], "entries");
        assertEqArr(sort(b.inverseEntries()).map((e) => [e[1], e[0]]), [["a", "1"], ["b", "2"]], "inverseEntries mirror");
        assertEqArr(sort([...b]), [["a", "1"], ["b", "2"]], "iterator");
    }],
    ["BiMap round trip", () => {
        const b = new BiMap();
        b.set("a", "1");
        const back = BiMap.deserialize(b.serialize());
        assertEq(back.get("a"), "1", "forward parity");
        assertEq(back.keyOf("1"), "a", "inverse parity");
    }],
]);

/* ================================================================== *
 *  Table — sparse string x string x value map.
 * ================================================================== */
runTable([
    ["put/get/has", [["put", "r1", "c1", 1], ["put", "r1", "c2", 2], ["put", "r2", "c1", 3], ["get", "r1", "c1"], ["get", "r1", "c3"], ["has", "r1", "c2"], ["has", "r3", "c1"], ["size"]], [SKIP, SKIP, SKIP, 1, undefined, true, false, 3]],
    ["delete one cell", [["put", "r1", "c1", 1], ["delete", "r1", "c1"], ["delete", "r1", "c1"], ["has", "r1", "c1"], ["size"]], [SKIP, true, false, false, 0]],
], () => new Table());

runBespoke([
    ["row/column/cells projections (order not pinned)", () => {
        const t = new Table();
        t.put("r1", "c1", 1); t.put("r1", "c2", 2); t.put("r2", "c1", 3);
        const sort2 = (xs) => xs.slice().sort((x, y) => (x[0] < y[0] ? -1 : 1));
        assertEqArr(sort2(t.row("r1")), [["c1", 1], ["c2", 2]], "row r1");
        assertEqArr(sort2(t.column("c1")), [["r1", 1], ["r2", 3]], "column c1");
        assertEq(t.cells().length, 3, "cells count");
        assertEq([...t].length, 3, "iterator count");
    }],
    ["Table round trip", () => {
        const t = new Table();
        t.put("r1", "c1", 1);
        assertEq(Table.deserialize(t.serialize()).get("r1", "c1"), 1, "get parity");
    }],
]);

/* ================================================================== *
 *  RangeSet — disjoint merged closed intervals.
 * ================================================================== */
runTable([
    ["disjoint adds stay separate", [["add", 1, 3], ["add", 5, 7], ["ranges"], ["size"]], [SKIP, SKIP, [[1, 3], [5, 7]], 2]],
    ["overlapping add merges", [["add", 1, 3], ["add", 2, 6], ["ranges"]], [SKIP, SKIP, [[1, 6]]]], // [1,3]∪[2,6]=[1,6]
    // DOC-TENSION resolved: d.ts/dynajs.d.ts prose says "closed", but the shipped
    // engine AND its own tests pin half-open [lo, hi) ("hi is exclusive",
    // test_structures_gaps.js). Module-consistent half-open reading used.
    ["contains includes lo, excludes hi", [["add", 1, 3], ["contains", 1], ["contains", 3], ["contains", 4], ["contains", 0]], [SKIP, true, false, false, false]],
    ["encloses", [["add", 1, 5], ["encloses", 1, 5], ["encloses", 2, 4], ["encloses", 1, 6], ["encloses", 0, 5]], [SKIP, true, true, false, false]],
    ["intersects", [["add", 1, 3], ["add", 5, 7], ["intersects", 2, 5], ["intersects", 3, 5], ["intersects", 4, 6], ["intersects", 6, 9], ["intersects", 7, 9], ["intersects", 8, 9]], [SKIP, SKIP, true, false, true, true, false, false]],
    // half-open gaps of [0,10) outside {[2,3),[6,7)}: [0,2) [3,6) [7,10)
    ["complement: gaps of [lo,hi) outside the set", [["add", 2, 3], ["add", 6, 7], ["complement", 0, 10], ["complement", 2, 3]], [SKIP, SKIP, [[0, 2], [3, 6], [7, 10]], []]],
    ["remove splits a range", [["add", 1, 7], ["remove", 3, 4], ["ranges"]], [SKIP, SKIP, [[1, 3], [4, 7]]]],
    ["clear empties", [["add", 1, 2], ["clear"], ["ranges"], ["size"]], [SKIP, undefined, [], 0]],
], () => new RangeSet());

runTable([
    // half-open adjacency: [1,3)+[3,6) share the boundary point 3 and merge
    ["adjacent half-open intervals merge", [["add", 1, 3], ["add", 3, 6], ["ranges"]], [SKIP, SKIP, [[1, 6]]]],
], () => new RangeSet());
// DOC-TENSION resolved (see above): [1,3]+[4,7] do NOT merge — under the
// module's half-open reading there is a real gap at [3,4).

runBespoke([
    ["measure is a positive total for a non-empty set", () => {
        const rs = new RangeSet();
        rs.add(1, 3);
        assert(typeof rs.measure === "number" && rs.measure > 0, "measure > 0 for [1,3]");
    }],
    ["RangeSet round trip", () => {
        const rs = new RangeSet();
        rs.add(1, 3); rs.add(5, 7);
        assertEqArr(RangeSet.deserialize(rs.serialize()).ranges(), [[1, 3], [5, 7]], "ranges parity");
    }],
]);

/* ================================================================== *
 *  RangeMap — interval to value, overlapping puts split.
 * ================================================================== */
runTable([
    ["put/get with gaps", [["put", 0, 10, "a"], ["put", 20, 30, "c"], ["get", 5], ["get", 15], ["get", 20], ["size"]], [SKIP, SKIP, "a", undefined, "c", 2]],
    // half-open: [5,25) covers up to but excluding 25, so get(25) is "c" (see RangeSet DOC-TENSION)
    ["overlapping put splits the old ranges", [["put", 0, 10, "a"], ["put", 20, 30, "c"], ["put", 5, 25, "b"], ["get", 3], ["get", 5], ["get", 15], ["get", 25], ["get", 26], ["size"]], [SKIP, SKIP, SKIP, "a", "b", "b", "c", "c", 3]],
    ["remove punches a hole", [["put", 0, 10, "a"], ["remove", 2, 4], ["get", 3], ["get", 1], ["get", 5]], [SKIP, SKIP, undefined, "a", "a"]],
], () => new RangeMap());

runBespoke([
    ["RangeMap round trip", () => {
        const rm = new RangeMap();
        rm.put(0, 10, "a"); rm.put(20, 30, "c"); rm.put(5, 25, "b");
        const back = RangeMap.deserialize(rm.serialize());
        assertEq(back.size, 3, "size parity");
        const es = back.entries().slice().sort((x, y) => x[0] - y[0]);
        assertEq(es.length, 3, "entries parity");
        assertEq(back.get(15), "b", "value parity");
    }],
]);

/* ================================================================== *
 *  IntervalTree — overlap enumeration, closed intervals.
 * ================================================================== */
runTable([
    ["insert and size", [["insert", 0, 10, "a"], ["insert", 5, 15, "b"], ["insert", 20, 30, "c"], ["size"]], [SKIP, SKIP, SKIP, 3]],
], () => new IntervalTree());

runBespoke([
    ["overlapping/at enumeration (set comparison, order not pinned)", () => {
        const t = new IntervalTree();
        t.insert(0, 10, "a"); t.insert(5, 15, "b"); t.insert(20, 30, "c");
        const labels = (xs) => xs.map((e) => e[2]).sort();
        assertEqArr(labels(t.overlapping(6, 7)), ["a", "b"], "overlapping(6,7)");
        assertEq(t.overlapping(16, 18).length, 0, "overlapping gap");
        assertEq(t.overlapping(0, 100).length, 3, "overlapping all");
        assertEqArr(labels(t.at(10)), ["a", "b"], "at a shared boundary");
        assertEqArr(labels(t.at(20)), ["c"], "at a lone start");
        assertEq(t.at(16).length, 0, "at a gap");
    }],
    ["IntervalTree round trip", () => {
        const t = new IntervalTree();
        t.insert(0, 10, "a"); t.insert(5, 15, "b");
        assertEq(IntervalTree.deserialize(t.serialize()).overlapping(6, 7).length, 2, "overlapping parity");
    }],
]);

/* ================================================================== *
 *  CountMinSketch — with only one distinct key added, count(key) is
 *  EXACT (no other key can inflate a counter); with several keys it is
 *  an upper-bound estimate, so only >= is pinned.
 * ================================================================== */
runTable([
    ["configured geometry", [["width"], ["depth"]], [32, 4]],
    ["single-key counts are exact", [["add", "x", 3], ["add", "x"], ["count", "x"], ["totalCount"], ["count", "y"]], [SKIP, SKIP, 4, 4, 0]],
], () => new CountMinSketch(32, 4));

runBespoke([
    ["multi-key estimates are >= true counts; totalCount is exact", () => {
        const cms = new CountMinSketch(32, 4);
        cms.add("x", 4); cms.add("y", 2);
        assertEq(cms.totalCount, 6, "totalCount is the exact sum");
        assert(cms.count("x") >= 4, "count(x) >= 4 under possible collisions");
    }],
    ["merge combines sketches", () => {
        const a = new CountMinSketch(32, 4);
        const b = new CountMinSketch(32, 4);
        a.add("x", 4);
        b.add("z", 10);
        a.merge(b);
        assertEq(a.totalCount, 14, "merged totalCount 4+10");
        assert(a.count("x") >= 4 && a.count("z") >= 10, "merged estimates >= true counts");
    }],
    ["CountMinSketch round trip", () => {
        const cms = new CountMinSketch(32, 4);
        cms.add("x", 4);
        assertEq(CountMinSketch.deserialize(cms.serialize()).count("x"), 4, "count parity (single key exact)");
    }],
]);

/* ================================================================== *
 *  HyperLogLog — cardinality estimator; exact expectations are only
 *  pinned where the estimator is exact (single element), otherwise
 *  determinism/monotonicity/merge-parity are pinned instead.
 * ================================================================== */
runBespoke([
    ["registers = 2^precision (standard layout)", () => {
        const h = new HyperLogLog();
        assertEq(h.registers, 2 ** h.precision, "registers from default precision");
        const h12 = new HyperLogLog(12);
        assertEq(h12.precision, 12, "precision property echoes the constructor arg");
    }],
    ["a single distinct key counts ~1", () => {
        const h = new HyperLogLog();
        h.add("k1");
        // the contract says only "Cardinality estimator" — no exactness at 1.
        // Textbook linear counting gives m*ln(m/(m-1)) ~ 1.0000305 for m=2^16.
        const c = h.count();
        assert(typeof c === "number" && Math.abs(c - 1) < 0.01, "singleton cardinality ~1, got " + c);
    }],
    ["counts are deterministic for identical inputs", () => {
        const build = () => { const h = new HyperLogLog(); h.add("a"); h.add("b"); h.add("c"); return h; };
        assertEq(build().count(), build().count(), "same inputs, same estimate");
    }],
    ["merge parity: union estimate equals a single sketch over the union", () => {
        const a = new HyperLogLog();
        a.add("a"); a.add("b");
        const b = new HyperLogLog();
        b.add("c");
        const whole = new HyperLogLog();
        whole.add("a"); whole.add("b"); whole.add("c");
        assertEq(a.merge(b).count(), whole.count(), "merge(a,b) == build(a+b)");
    }],
    ["count never decreases as keys are added", () => {
        const h = new HyperLogLog();
        h.add("a");
        const c1 = h.count();
        h.add("b"); h.add("c"); h.add("d");
        assert(h.count() >= c1, "monotone in adds");
    }],
    ["HyperLogLog round trip", () => {
        const h = new HyperLogLog();
        h.add("a"); h.add("b");
        assertEq(HyperLogLog.deserialize(h.serialize()).count(), h.count(), "estimate parity");
    }],
]);

/* ================================================================== *
 *  Documented refusal table (constructor strictness lives with the
 *  classes above; collected here for the row count).
 * ================================================================== */
runBespoke([
    ["Graph constructor rejects unknown option keys with TypeError naming the key", () => {
        assertThrows(() => new Graph({ bogus: 1 }), "strict options bag", TypeError, /bogus/);
    }],
]);

/* ================================================================== *
 *  UnionFind DYNS codec hardening. d.ts: "static deserialize(bytes):
 *  Uint8Array | ArrayBuffer): UnionFind" over a "Disjoint-set forest
 *  with path halving and union by rank" — a record whose parent graph
 *  contains a cycle has no forest semantics, so deserialize must
 *  refuse it with the codec's corruption error instead of producing
 *  an object whose find()/union()/connected() never terminate.
 * ================================================================== */
{
    const CRC32C_T = (() => {
        const t = new Uint32Array(256);
        for (let i = 0; i < 256; i++) {
            let c = i;
            for (let k = 0; k < 8; k++) c = (c & 1) ? (0x82F63B78 ^ (c >>> 1)) : (c >>> 1);
            t[i] = c >>> 0;
        }
        return t;
    })();
    const crc32c = (bytes) => {
        let c = 0xFFFFFFFF;
        for (let i = 0; i < bytes.length; i++) c = CRC32C_T[(c ^ bytes[i]) & 0xFF] ^ (c >>> 8);
        return (c ^ 0xFFFFFFFF) >>> 0;
    };
    const ufRecord = (payload) => {
        const len = 20 + payload.length + 4;
        const b = new Uint8Array(len);
        b[0] = 0x44; b[1] = 0x59; b[2] = 0x4E; b[3] = 0x53;
        b[4] = 1;
        b[6] = 2;
        b[12] = payload.length & 0xFF;
        b[13] = (payload.length >>> 8) & 0xFF;
        b.set(payload, 20);
        const crc = crc32c(b.subarray(0, len - 4));
        for (let i = 0; i < 4; i++) b[len - 4 + i] = (crc >>> (i * 8)) & 0xFF;
        return b;
    };
    runBespoke([
        ["UnionFind.serialize/deserialize round trip stays a forest (d.ts: find/union/connected over n elements)", () => {
            const u = new UnionFind(4);
            u.union(0, 1);
            const v = UnionFind.deserialize(u.serialize());
            assertEq(v.count, 3, "two components after one union: 4 elements, 3 roots");
            assertEq(v.connected(0, 1), true, "union survived the round trip");
            assertEq(v.connected(2, 3), false, "unrelated elements stay apart");
        }],
        ["UnionFind.deserialize refuses a cyclic DELTA parent graph (d.ts deserialize: records are DYNS; cyclic parents are not a forest)", () => {
            const payload = Uint8Array.of(
                0xFF, 0xFF, 0xFF, 0xFF,
                2, 0, 0, 0,
                2, 0, 0, 0,
                1,
                0x02,
                0x01,
                0x02, 0x00);
            assertThrows(() => UnionFind.deserialize(ufRecord(payload)),
                "parent[0]=1, parent[1]=0 must be refused as a corrupted record",
                TypeError, /truncated DYNS record/);
        }],
        ["UnionFind.deserialize refuses a cyclic RAW parent graph (d.ts deserialize: records are DYNS; cyclic parents are not a forest)", () => {
            const payload = Uint8Array.of(
                2, 0, 0, 0,
                2, 0, 0, 0,
                1, 0,
                0, 0,
                0, 0, 0, 0, 0, 0);
            assertThrows(() => UnionFind.deserialize(ufRecord(payload)),
                "raw parent[0]=1, parent[1]=0 must be refused as a corrupted record",
                TypeError, /truncated DYNS record/);
        }],
        ["UnionFind.deserialize keeps the root count truthful (d.ts: count is the number of disjoint components)", () => {
            const u = new UnionFind(3);
            u.union(0, 1); u.union(1, 2);
            const v = UnionFind.deserialize(u.serialize());
            assertEq(v.count, 1, "chained unions collapse to one component");
            assertEq(v.find(2), v.find(0), "find(2) and find(0) agree after the round trip");
        }],
    ]);
}

print("bb_structures: all tests passed (" + n + " assertions)");
