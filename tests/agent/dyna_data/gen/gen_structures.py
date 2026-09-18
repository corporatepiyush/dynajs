#!/usr/bin/env python3
"""gen_structures.py — dyna:structures probes.

Oracles: in-probe reference models (JS arrays/Maps driven by the same seeded
op trace) for the linear/sorted/keyed containers, and python-computed graph
fixtures (BFS/DFS/Dijkstra/Bellman-Ford/Floyd-Warshall/MST partitions/CC
partitions) baked as literals.
"""
import heapq
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import write_probe


def gen_graph(seed, n, p, directed=False, weighted=False, wmin=1, wmax=9):
    """Deterministic small graph. Returns nodes/edges + expected analytics."""
    edges = []
    a = seed
    def rnd():
        nonlocal a
        a = (a * 1103515245 + 12345) & 0xFFFFFFFF
        return a >> 8
    for u in range(n):
        for v in range(n):
            if not directed and v < u:
                continue
            if u == v:
                continue
            if rnd() % 100 < p * 100:
                w = wmin + rnd() % (wmax - wmin + 1) if weighted else 1
                edges.append((u, v, w))
                if not directed:
                    pass  # addEdge adds both directions for undirected
    adj = {u: [] for u in range(n)}
    for (u, v, w) in edges:
        adj[u].append((v, w))
        if not directed:
            adj[v].append((u, w))
    return n, edges, adj


def bfs_order(n, adj):
    from collections import deque
    seen = [False] * n
    seen[0] = True
    q = deque([0])
    out = []
    while q:
        u = q.popleft()
        out.append(u)
        for (v, _) in adj[u]:
            if not seen[v]:
                seen[v] = True
                q.append(v)
    return out


def dfs_order(n, adj):
    seen = [False] * n
    out = []
    def go(u):
        seen[u] = True
        out.append(u)
        for (v, _) in sorted(adj[u]):
            if not seen[v]:
                go(v)
    go(0)
    return out


def dijkstra(n, adj, src):
    INF = float("inf")
    dist = [INF] * n
    dist[src] = 0
    pq = [(0, src)]
    while pq:
        d, u = heapq.heappop(pq)
        if d > dist[u]:
            continue
        for (v, w) in adj[u]:
            nd = d + w
            if nd < dist[v]:
                dist[v] = nd
                heapq.heappush(pq, (nd, v))
    return dist


def bellmanford(n, edges, src, directed):
    INF = float("inf")
    dist = [INF] * n
    dist[src] = 0
    alledges = list(edges)
    if not directed:
        alledges += [(v, u, w) for (u, v, w) in edges]
    for _ in range(n):
        for (u, v, w) in alledges:
            if dist[u] + w < dist[v]:
                dist[v] = dist[u] + w
    return dist


def floyd(n, edges, directed):
    INF = float("inf")
    d = [[INF] * n for _ in range(n)]
    for i in range(n):
        d[i][i] = 0
    alledges = list(edges)
    if not directed:
        alledges += [(v, u, w) for (u, v, w) in edges]
    for (u, v, w) in alledges:
        if w < d[u][v]:
            d[u][v] = w
    for k in range(n):
        for i in range(n):
            for j in range(n):
                if d[i][k] + d[k][j] < d[i][j]:
                    d[i][j] = d[i][k] + d[k][j]
    return d


def mst_weight(n, edges):
    parent = list(range(n))
    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x
    total = 0
    for (u, v, w) in sorted(edges, key=lambda e: e[2]):
        ru, rv = find(u), find(v)
        if ru != rv:
            parent[ru] = rv
            total += w
    return total


def components(n, edges, directed):
    parent = list(range(n))
    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x
    alledges = list(edges)
    if not directed:
        pass
    for (u, v, _) in edges:
        ru, rv = find(u), find(v)
        if ru != rv:
            parent[ru] = rv
    groups = {}
    for u in range(n):
        groups.setdefault(find(u), []).append(u)
    return sorted(sorted(g) for g in groups.values())


def probe_graph():
    """Graph analytics vs python fixtures on deterministic random graphs."""
    fixtures = []
    specs = [
        (11, 8, 0.30, True, True, 1, 9, 0),     # directed weighted
        (12, 7, 0.35, False, True, 1, 9, 1),    # undirected weighted
        (10, 9, 0.30, True, False, 1, 1, 2),    # directed unweighted
        (9, 10, 0.40, False, False, 1, 1, 3),   # undirected unweighted
    ]
    for (seed, n, p, directed, weighted, wmin, wmax, tag) in specs:
        n2, edges, adj = gen_graph(seed, n, p, directed, weighted, wmin, wmax)
        bfs = bfs_order(n2, adj)
        dfs = dfs_order(n2, adj)
        dij = dijkstra(n2, adj, 0)
        if weighted:
            bf = bellmanford(n2, edges, 0, directed)
            fw = floyd(n2, edges, directed)
        else:
            bf = dij
            fw = None
        cc = components(n2, edges, directed)
        mw = mst_weight(n2, edges) if (not directed and weighted) else None
        # undirected engine graphs store ONE addEdge call per pair; neighbors
        # of u include v for every stored pair either way.
        fixture = {
            "n": n2, "directed": directed, "weighted": weighted,
            "edges": edges, "bfs": bfs, "dfs": dfs, "dij": dij, "bf": bf,
            "cc": cc, "mw": mw,
            "fw": [[None if x == float("inf") else x for x in row] for row in fw] if fw else None,
        }
        fixtures.append(fixture)

    emit = ['import { Graph } from "dyna:structures";']
    emit.append("var FIX = [")
    for f in fixtures:
        edges = ",".join("[%d,%d,%d]" % e for e in f["edges"])
        dij = ",".join("Infinity" if x == float("inf") else repr(x) for x in f["dij"])
        bf = ",".join("Infinity" if x == float("inf") else repr(x) for x in f["bf"])
        bfs = ",".join(str(x) for x in f["bfs"])
        dfs = ",".join(str(x) for x in f["dfs"])
        cc = ";".join(",".join(str(x) for x in g) for g in f["cc"])
        fw = "null"
        if f["fw"]:
            fw = ";".join(",".join("Infinity" if x is None else repr(x) for x in row) for row in f["fw"])
        mw = "null" if f["mw"] is None else repr(f["mw"])
        emit.append('  {n: %d, directed: %s, weighted: %s, edges: [%s], bfs: [%s], dfs: [%s], dij: [%s], bf: [%s], cc: "%s", fw: "%s", mw: %s},'
                    % (f["n"], str(f["directed"]).lower(), str(f["weighted"]).lower(),
                       edges, bfs, dfs, dij, bf, cc, fw, mw))
    emit.append("];")

    emit.append(r"""
function mkGraph(f) {
  var g = new Graph({ directed: f.directed, weighted: f.weighted });
  for (var i = 0; i < f.n; i++) g.addNode();
  for (var i = 0; i < f.edges.length; i++) g.addEdge(f.edges[i][0], f.edges[i][1], f.edges[i][2]);
  return g;
}
for (var fi = 0; fi < FIX.length; fi++) {
  (function (f, tag) {
    var g = mkGraph(f);
    assert_eq(g.nodeCount, f.n, tag + " nodeCount");
    assert_eq(g.edgeCount, f.edges.length, tag + " edgeCount");
    // bfs/dfs from node 0 (0 is always present)
    assert_eq(JSON.stringify(g.bfs(0)), JSON.stringify(f.bfs), tag + " bfs");
    assert_eq(JSON.stringify(g.dfs(0)), JSON.stringify(f.dfs), tag + " dfs");
    var d = g.dijkstra(0);
    assert_eq(d.length, f.dij.length, tag + " dij length");
    for (var i = 0; i < d.length; i++) assert_eq(d[i], f.dij[i], tag + " dij[" + i + "]");
    var bf = g.bellmanFord(0);
    for (var i = 0; i < bf.length; i++) assert_eq(bf[i], f.bf[i], tag + " bf[" + i + "]");
    // connected components: same PARTITION (labels may differ)
    var cc = g.connectedComponents();
    var groups = {};
    for (var i = 0; i < cc.length; i++) (groups[cc[i]] = groups[cc[i]] || []).push(i);
    var norm = Object.keys(groups).map(function (k) { return groups[k].join(","); }).sort().join(";");
    assert_eq(norm, f.cc, tag + " cc partition");
    if (f.weighted) {
      var fw = g.floydWarshall();
      var rows = f.fw.split(";").map(function (r) { return r.split(",").map(Number); });
      for (var i = 0; i < fw.length; i++)
        for (var j = 0; j < fw[i].length; j++)
          assert_eq(fw[i][j], rows[i][j], tag + " fw[" + i + "][" + j + "]");
    }
    if (!f.directed && f.weighted) {
      var m = g.mst();
      assert_eq(m.weight, f.mw, tag + " mst weight");
      assert_eq(m.edges.length, f.n - 1, tag + " mst edge count");
    }
    // serialize round-trip preserves analytics
    var g2 = Graph.deserialize(g.serialize());
    assert_eq(JSON.stringify(g2.dijkstra(0)), JSON.stringify(d), tag + " ser rt dijkstra");
  })(FIX[fi], "fix" + fi);
}
// pinned micro-cases with unique orders
var chain = new Graph({ directed: true, weighted: true });
var a = chain.addNode(), b = chain.addNode(), c = chain.addNode();
chain.addEdge(a, b, 2).addEdge(b, c, 3);
assert_eq(JSON.stringify(chain.topologicalSort()), "[0,1,2]", "topo chain");
assert_eq(JSON.stringify(chain.dijkstra(a)), "[0,2,5]", "chain dijkstra");
var ast = chain.aStar(a, c, function (n2) { return Math.abs(n2 - c); });
assert_eq(ast.dist, 5, "aStar dist");
assert_eq(JSON.stringify(ast.path), "[0,1,2]", "aStar path");
// refusals documented
var cyc = new Graph({ directed: true });
var x1 = cyc.addNode(), x2 = cyc.addNode();
cyc.addEdge(x1, x2); cyc.addEdge(x2, x1);
assert_throws(function () { cyc.topologicalSort(); }, "RangeError", "topo cycle");
var und = new Graph();
und.addNode();
assert_throws(function () { und.topologicalSort(); }, "TypeError", "topo undirected");
var dirg = new Graph({ directed: true });
dirg.addNode();
assert_throws(function () { dirg.mst(); }, "TypeError", "mst directed check");
var negg = new Graph({ directed: true, weighted: true });
var y1 = negg.addNode(), y2 = negg.addNode();
negg.addEdge(y1, y2, -1);
assert_throws(function () { negg.dijkstra(y1); }, "RangeError", "dijkstra negative edge");
assert_throws(function () { negg.addEdge(y1, y2, Infinity); }, "RangeError", "non-finite weight");
summary("structures_graph");
""")
    return write_probe("structures", "graph", "\n".join(emit))


def probe_linear():
    emit = ['import { Deque, List, RingBuffer, Heap, MinMaxHeap } from "dyna:structures";']
    emit.append(r"""
function lcgGen(seed) { var a = seed >>> 0; return function () { a = (Math.imul(a, 1103515245) + 12345) >>> 0; return a >>> 1; }; }
// ---- Deque vs array model under a random op trace
var bad = 0, rnd = lcgGen(7);
var d = new Deque(), model = [];
for (var step = 0; step < 4000; step++) {
  var op = rnd() % 6;
  var v = rnd() % 1000;
  if (op === 0) { d.pushBack(v); model.push(v); }
  else if (op === 1) { d.pushFront(v); model.unshift(v); }
  else if (op === 2) { var g1 = d.popFront(), g2 = model.shift(); if (g1 !== g2) bad++; }
  else if (op === 3) { var g3 = d.popBack(), g4 = model.pop(); if (g3 !== g4) bad++; }
  else if (op === 4) { var idx = rnd() % Math.max(1, model.length); if (d.get(idx) !== model[idx]) bad++; }
  else { if (d.length !== model.length) bad++; }
}
if (bad) { __fail++; __out("FAIL deque model violations " + bad); } else __pass++;
// final state agrees element-wise
var da = d.toArray();
assert_eq(JSON.stringify(da), JSON.stringify(model), "deque final toArray");
var iter = [];
for (var x of d) iter.push(x);
assert_eq(JSON.stringify(iter), JSON.stringify(model), "deque iterate");
// OOB get is undefined
assert_eq(d.get(model.length), undefined, "deque get at end undefined");
assert_eq(d.get(-1), undefined, "deque get -1 undefined");
// ---- List
var l = new List(), lm = [];
l.pushBack(1); lm.push(1);
l.pushFront(0); lm.unshift(0);
l.pushBack(2); lm.push(2);
assert_eq(JSON.stringify(l.toArray()), JSON.stringify(lm), "list toArray");
assert_eq(l.front(), lm[0], "list front");
assert_eq(l.back(), lm[lm.length - 1], "list back");
assert_eq(l.popFront(), lm.shift(), "list popFront");
assert_eq(l.popBack(), lm.pop(), "list popBack");
assert_eq(l.length, lm.length, "list length");
// ---- RingBuffer: fixed capacity, evicts oldest, iterates oldest-first
var rb = new RingBuffer(3);
rb.push(1); rb.push(2); rb.push(3);
assert_eq(JSON.stringify(rb.toArray()), "[1,2,3]", "ring before wrap");
rb.push(4);
assert_eq(JSON.stringify(rb.toArray()), "[2,3,4]", "ring evicts oldest");
assert_eq(rb.length, 3, "ring length capped");
assert_eq(rb.capacity, 3, "ring capacity");
assert_eq(rb.full, true, "ring full flag");
assert_eq(rb.get(0), 2, "ring get 0 = oldest");
var acc = [];
for (var x of rb) acc.push(x);
assert_eq(JSON.stringify(acc), "[2,3,4]", "ring iterate");
assert_eq(rb.get(3), undefined, "ring get OOB undefined");
assert_throws(function () { new RingBuffer(0); }, "RangeError", "ring capacity 0");
// ---- Heap: min-heap by default; pops come out sorted
bad = 0;
var h = new Heap(), hm = [];
for (var step2 = 0; step2 < 2000; step2++) {
  var op2 = rnd() % 3, v2 = rnd() % 10000;
  if (op2 !== 0 || hm.length === 0) { h.push(v2); hm.push(v2); }
  else { var p1 = h.pop(), p2 = Math.min.apply(null, hm); if (p1 !== p2) bad++; hm.splice(hm.indexOf(p2), 1); }
  if (hm.length && h.peek() !== Math.min.apply(null, hm)) bad++;
  if (h.size !== hm.length) bad++;
}
if (bad) { __fail++; __out("FAIL heap model violations " + bad); } else __pass++;
var sortedPops = [];
while (h.size) sortedPops.push(h.pop());
assert_eq(JSON.stringify(sortedPops), JSON.stringify(hm.slice().sort(function (a, b) { return a - b; })), "heap drains sorted");
assert_eq(new Heap().pop(), undefined, "heap pop empty undefined");
// custom comparator = max-heap
var mh = new Heap(function (x, y) { return y - x; });
mh.push(1); mh.push(9); mh.push(5);
assert_eq(mh.pop(), 9, "max-heap pops biggest");
// ---- MinMaxHeap: both ends poppable in order
var mm = new MinMaxHeap();
[5, 1, 9, 3, 7].forEach(function (v) { mm.push(v); });
assert_eq(mm.size, 5, "mm size");
assert_eq(mm.peekMin(), 1, "mm peekMin");
assert_eq(mm.peekMax(), 9, "mm peekMax");
assert_eq(mm.popMin(), 1, "mm popMin");
assert_eq(mm.popMax(), 9, "mm popMax");
assert_eq(mm.size, 3, "mm size after pops");
assert_eq(mm.popMax(), 7, "mm popMax 2");
assert_eq(mm.popMin(), 3, "mm popMin 2");
assert_eq(mm.popMax(), 5, "mm popMax 3");
assert_eq(mm.popMin(), undefined, "mm empty");
// ---- serialize round-trips
var d2 = new Deque();
[1, "x", null, 4].forEach(function (v) { d2.pushBack(v); });
var d3 = Deque.deserialize(d2.serialize());
assert_eq(JSON.stringify(d3.toArray()), JSON.stringify(d2.toArray()), "deque ser rt");
var h2 = new Heap();
[2, 1, 3].forEach(function (v) { h2.push(v); });
var h3 = Heap.deserialize(h2.serialize());
assert_eq(h3.pop(), 1, "heap ser rt pop");
var mm2 = new MinMaxHeap();
[1, 8, 4].forEach(function (v) { mm2.push(v); });
var mm3 = MinMaxHeap.deserialize(mm2.serialize());
assert_eq(mm3.popMin(), 1, "mm ser rt");
var rb2 = new RingBuffer(4);
[1, 2, 3].forEach(function (v) { rb2.push(v); });
var rb3 = RingBuffer.deserialize(rb2.serialize());
assert_eq(JSON.stringify(rb3.toArray()), "[1,2,3]", "ring ser rt");
var l2 = new List();
[7, 8].forEach(function (v) { l2.pushBack(v); });
var l3 = List.deserialize(l2.serialize());
assert_eq(JSON.stringify(l3.toArray()), "[7,8]", "list ser rt");
summary("structures_linear");
""")
    return write_probe("structures", "linear", "\n".join(emit))


def probe_sorted():
    emit = ['import { SortedSet, SortedMap, BTree } from "dyna:structures";']
    emit.append(r"""
function lcgGen(seed) { var a = seed >>> 0; return function () { a = (Math.imul(a, 1103515245) + 12345) >>> 0; return a >>> 1; }; }
function modelFloor(arr, x) { var f; for (var i = 0; i < arr.length; i++) if (arr[i] <= x) f = arr[i]; return f; }
function modelCeil(arr, x) { for (var i = 0; i < arr.length; i++) if (arr[i] >= x) return arr[i]; }
// ---- SortedSet randomized vs sorted-array model
var rnd = lcgGen(21);
var s = new SortedSet(), model = [];
var bad = 0;
for (var step = 0; step < 3000; step++) {
  var v = rnd() % 400;
  var op = rnd() % 4;
  if (op < 2) {
    var added = model.indexOf(v) === -1;
    s.add(v);
    if (added && model.indexOf(v) === -1) { model.push(v); model.sort(function (a, b) { return a - b; }); }
  } else if (op === 2) {
    var had = model.indexOf(v) !== -1;
    if (s.delete(v) !== had) bad++;
    var k = model.indexOf(v);
    if (had) model.splice(k, 1);
  } else {
    if (s.has(v) !== (model.indexOf(v) !== -1)) bad++;
  }
  if (s.size !== model.length) bad++;
}
if (bad) { __fail++; __out("FAIL sortedset model violations " + bad); } else __pass++;
assert_eq(JSON.stringify(s.toArray()), JSON.stringify(model), "sortedset final order");
var iter = [];
for (var x of s) iter.push(x);
assert_eq(JSON.stringify(iter), JSON.stringify(model), "sortedset iterate");
// floor/ceil vs model
bad = 0;
for (var q = -5; q < 405; q += 7) {
  if (s.floor(q) !== modelFloor(model, q)) bad++;
  if (s.ceil(q) !== modelCeil(model, q)) bad++;
}
if (bad) { __fail++; __out("FAIL floor/ceil violations " + bad); } else __pass++;
// floor/ceil conventions from the doc examples
var ss = new SortedSet();
[1, 3, 5].forEach(function (x) { ss.add(x); });
assert_eq(ss.floor(4), 3, "floor below");
assert_eq(ss.floor(1), 1, "floor exact");
assert_eq(ss.ceil(4), 5, "ceil above");
assert_eq(ss.ceil(5), 5, "ceil exact");
assert_eq(ss.floor(0), undefined, "floor none undefined");
assert_eq(ss.ceil(6), undefined, "ceil none undefined");
assert_eq(JSON.stringify(ss.rangeQuery(2, 5)), "[3,5]", "rangeQuery inclusive");
assert_eq(ss.first(), 1, "first");
assert_eq(ss.last(), 5, "last");
// ---- SortedMap: number keys, ordered
var m = new SortedMap(), mm = {};
m.set(2, "a"); m.set(1, "b"); m.set(3, "c");
assert_eq(JSON.stringify(m.keys()), "[1,2,3]", "sortedmap keys ordered");
assert_eq(m.get(2), "a", "sortedmap get");
assert_eq(m.firstKey(), 1, "sortedmap firstKey");
assert_eq(m.lastKey(), 3, "sortedmap lastKey");
assert_eq(m.floorKey(2), 2, "sortedmap floorKey exact");
assert_eq(m.ceilKey(4), undefined, "sortedmap ceilKey none");
assert_eq(m.has(3), true, "sortedmap has");
assert_eq(m.delete(3), true, "sortedmap delete");
assert_eq(m.delete(99), false, "sortedmap delete miss");
assert_eq(m.size, 2, "sortedmap size");
assert_eq(JSON.stringify(m.rangeQuery(1, 2)), "[[1,\"b\"],[2,\"a\"]]", "sortedmap range");
assert_throws(function () { m.set("b", 1); }, null, "sortedmap string key refused");
// ---- BTree: larger-scale sorted behavior
var bt = new BTree();
for (var i = 0; i < 2000; i++) bt.set((i * 37) % 2000, i);
assert_eq(bt.size, 2000, "btree size (all distinct)");
assert_eq(bt.firstKey(), 0, "btree firstKey");
assert_eq(bt.lastKey(), 1999, "btree lastKey");
var hit = 0, miss = 0;
for (var i = 0; i < 2000; i++) { if (bt.get(i) !== undefined) hit++; else miss++; }
assert_eq(hit, 2000, "btree gets hit");
assert_eq(bt.has(1500), true, "btree has");
assert_eq(bt.delete(1500), true, "btree delete");
assert_eq(bt.has(1500), false, "btree deleted");
assert_eq(bt.size, 1999, "btree size after delete");
assert_eq(bt.floorKey(1500), 1499, "btree floorKey after delete");
assert_eq(bt.ceilKey(1500), 1501, "btree ceilKey after delete");
var keys = bt.keys();
var sortedOK = true;
for (var i = 1; i < keys.length; i++) if (keys[i] <= keys[i - 1]) sortedOK = false;
assert_eq(sortedOK, true, "btree keys sorted");
assert_throws(function () { bt.set("x", 1); }, null, "btree string key refused");
// ---- 100k stress: size accounting + spot checks + full iteration (bounded)
var big = new BTree();
for (var i = 0; i < 100000; i++) big.set(i * 3, i);
assert_eq(big.size, 100000, "btree 100k size");
assert_eq(big.get(299997), 99999, "btree 100k spot get");
assert_eq(big.firstKey(), 0, "btree 100k first");
assert_eq(big.lastKey(), 299997, "btree 100k last");
var count = 0;
for (var k of big) { count++; if (count > 100000) break; }
assert_eq(count, 100000, "btree 100k iterate count");
assert_eq(JSON.stringify(big.rangeQuery(3, 9)), "[[3,1],[6,2],[9,3]]", "btree 100k range");
var big2 = BTree.deserialize(big.serialize());
assert_eq(big2.size, 100000, "btree 100k ser rt size");
assert_eq(big2.get(150000), 50000, "btree 100k ser rt get");
var smap = new SortedMap();
for (var i = 0; i < 100000; i++) smap.set(i, i * 2);
assert_eq(smap.size, 100000, "sortedmap 100k size");
assert_eq(smap.get(77777), 155554, "sortedmap 100k spot");
var sset = new SortedSet();
for (var i = 0; i < 100000; i++) sset.add(i);
assert_eq(sset.size, 100000, "sortedset 100k size");
assert_eq(sset.rangeQuery(10, 19).length, 10, "sortedset 100k range");
summary("structures_sorted");
""")
    return write_probe("structures", "sorted", "\n".join(emit))


def probe_keyed():
    emit = ['import { LRU, Trie, Multiset, Multimap, BiMap, Table } from "dyna:structures";']
    emit.append(r"""
function lcgGen(seed) { var a = seed >>> 0; return function () { a = (Math.imul(a, 1103515245) + 12345) >>> 0; return a >>> 1; }; }
// ---- LRU: capacity, recency on get, delete, serialize
var lru = new LRU(2);
lru.put("a", 1); lru.put("b", 2);
assert_eq(lru.get("a"), 1, "lru get a");
lru.put("c", 3);                     // evicts b (a was refreshed)
assert_eq(lru.has("b"), false, "lru evicted b");
assert_eq(lru.has("a"), true, "lru kept a");
assert_eq(lru.size, 2, "lru size");
assert_eq(lru.delete("a"), true, "lru delete true");
assert_eq(lru.delete("nope"), false, "lru delete miss");
assert_eq(lru.capacity, 2, "lru capacity getter");
var stats = lru.stats;
assert_eq(typeof stats === "object" && stats !== null, true, "lru stats object");
// TTL expiry on the monotonic clock: 5ms TTL
var lt = new LRU(4);
lt.setWithTTL("t", 1, 5);
assert_eq(lt.get("t"), 1, "ttl fresh");
var spin = 0;
while (lt.has("t") && spin < 2e7) spin++;
assert_eq(lt.has("t"), false, "ttl expires (spin " + spin + ")");
assert_eq(lt.purgeExpired(), 1, "purge removes the expired entry");
// ---- Trie: prefix set with embedded NUL as an ordinary byte
var tr = new Trie();
tr.insert("abc"); tr.insert("abcd"); tr.insert("abx"); tr.insert("a\x00b");
assert_eq(tr.has("abc"), true, "trie has");
assert_eq(tr.has("ab"), false, "trie non-word prefix");
assert_eq(tr.has("a\x00b"), true, "trie embedded NUL");
assert_eq(tr.size, 4, "trie size");
assert_eq(JSON.stringify(tr.keysWithPrefix("abc").sort()), "[\"abc\",\"abcd\"]", "trie prefix");
assert_eq(tr.keysWithPrefix("zzz").length, 0, "trie prefix none");
assert_eq(tr.longestPrefix("abcdxxx"), "abcd", "trie longestPrefix");
assert_eq(tr.longestPrefix("nope"), "", "trie longestPrefix none");
assert_eq(tr.delete("abcd"), true, "trie delete");
assert_eq(tr.has("abcd"), false, "trie deleted");
assert_eq(tr.delete("abcd"), false, "trie delete miss");
var trieIter = [];
for (var k of tr) trieIter.push(k);
assert_eq(trieIter.length, tr.size, "trie iterate count");
var tr2 = Trie.deserialize(tr.serialize());
assert_eq(tr2.has("a\x00b"), true, "trie ser rt NUL");
assert_eq(tr2.size, tr.size, "trie ser rt size");
// ---- Multiset: counts, remove clamps at zero
var ms = new Multiset();
assert_eq(ms.add("x"), 1, "ms add default 1");
assert_eq(ms.add("x", 2), 3, "ms add n=2");
assert_eq(ms.count("x"), 3, "ms count");
assert_eq(ms.has("x"), true, "ms has");
assert_eq(ms.remove("x", 5), 0, "ms remove clamps to 0");
assert_eq(ms.count("x"), 0, "ms count after clamp");
assert_eq(ms.has("x"), false, "ms has false at 0");
assert_eq(ms.size, 0, "ms size excludes zeros");
ms.add("a", 3); ms.add("b", 1);
assert_eq(ms.totalSize, 4, "ms totalSize");
assert_eq(ms.size, 2, "ms size distinct");
assert_eq(JSON.stringify(ms.elementSet().sort()), "[\"a\",\"b\"]", "ms elementSet");
assert_eq(ms.setCount("a", 2) === ms, true, "ms setCount returns this");
assert_eq(ms.count("a"), 2, "ms setCount applied");
assert_eq(ms.delete("a"), true, "ms delete");
assert_eq(JSON.stringify(ms.entrySet()), "[[\"b\",1]]", "ms entrySet");
ms.clear();
assert_eq(ms.size, 0, "ms clear");
var msIter = [];
for (var e of ms) msIter.push(e);
// ---- Multimap
var mm = new Multimap();
mm.put("k", 1); mm.put("k", 2); mm.put("j", 3);
assert_eq(JSON.stringify(mm.get("k")), "[1,2]", "mm get");
assert_eq(mm.count("k"), 2, "mm count");
assert_eq(mm.keyCount, 2, "mm keyCount");
assert_eq(mm.size, 3, "mm size (values)");
assert_eq(mm.removeAt("k", 0), 1, "mm removeAt");
assert_eq(JSON.stringify(mm.get("k")), "[2]", "mm after removeAt");
assert_eq(mm.delete("k"), 1, "mm delete returns remaining count removed");
assert_eq(mm.get("k").length, 0, "mm get empty after delete");
assert_eq(JSON.stringify(mm.entries()), "[[\"j\",3]]", "mm entries");
var mmIter = [];
for (var e of mm) mmIter.push(e);
assert_eq(mmIter.length, 1, "mm iterate");
// ---- BiMap: strict two-way uniqueness
var bi = new BiMap();
bi.set("k1", "v1");
assert_eq(bi.get("k1"), "v1", "bi get");
assert_eq(bi.keyOf("v1"), "k1", "bi keyOf");
assert_eq(bi.hasValue("v1"), true, "bi hasValue");
assert_throws(function () { bi.set("k2", "v1"); }, "TypeError", "bi duplicate value refused");
bi.forceSet("k2", "v1");
assert_eq(bi.has("k1"), false, "bi forceSet evicted old key");
assert_eq(bi.keyOf("v1"), "k2", "bi forceSet rebound");
bi.set("k3", "v3");
assert_eq(bi.delete("k3"), true, "bi delete");
assert_eq(bi.hasValue("v3"), false, "bi delete removed value too");
bi.set("k4", "v4");
assert_eq(bi.deleteValue("v4"), true, "bi deleteValue");
assert_eq(bi.has("k4"), false, "bi deleteValue removed key");
bi.set("k5", "v5"); bi.set("k6", "v6");
assert_eq(JSON.stringify(bi.entries().length), "3", "bi entries count");
assert_eq(bi.inverseEntries().length, 3, "bi inverseEntries");
assert_eq(bi.size, 3, "bi size");
bi.clear();
assert_eq(bi.size, 0, "bi clear");
// ---- Table: (row, col) -> value
var tb = new Table();
tb.put("r1", "c1", 5); tb.put("r1", "c2", 6); tb.put("r2", "c1", 7);
assert_eq(tb.get("r1", "c1"), 5, "table get");
assert_eq(tb.has("r2", "c1"), true, "table has");
assert_eq(tb.size, 3, "table size");
assert_eq(JSON.stringify(tb.row("r1")), "[[\"c1\",5],[\"c2\",6]]", "table row");
assert_eq(JSON.stringify(tb.column("c1")), "[[\"r1\",5],[\"r2\",7]]", "table column");
assert_eq(tb.cells().length, 3, "table cells");
assert_eq(tb.delete("r2", "c1"), true, "table delete");
assert_eq(tb.delete("r2", "c1"), false, "table delete miss");
var tbIter = [];
for (var e of tb) tbIter.push(e);
assert_eq(tbIter.length, tb.size, "table iterate");
summary("structures_keyed");
""")
    return write_probe("structures", "keyed", "\n".join(emit))


def probe_bits_prob():
    emit = ['import { BitSet, UnionFind, Fenwick, SegTree, BloomFilter,',
            '         CountMinSketch, HyperLogLog, RangeSet, RangeMap, IntervalTree } from "dyna:structures";']
    emit.append(r"""
function lcgGen(seed) { var a = seed >>> 0; return function () { a = (Math.imul(a, 1103515245) + 12345) >>> 0; return a >>> 1; }; }
// ---- BitSet randomized vs word model
var rnd = lcgGen(33);
var N = 500;
var bs = new BitSet(N), model = [];
for (var i = 0; i < N; i++) model.push(0);
var bad = 0;
for (var step = 0; step < 4000; step++) {
  var i = rnd() % N, op = rnd() % 5;
  if (op === 0) { bs.set(i); model[i] = 1; }
  else if (op === 1) { bs.clear(i); model[i] = 0; }
  else if (op === 2) { bs.flip(i); model[i] ^= 1; }
  else if (op === 3) { if (bs.get(i) !== (model[i] === 1)) bad++; }
  else {
    // nextSet consistency
    var want = -1;
    for (var j = i; j < N; j++) if (model[j]) { want = j; break; }
    if (bs.nextSet(i) !== want) bad++;
  }
}
if (bad) { __fail++; __out("FAIL bitset model violations " + bad); } else __pass++;
var ones = [];
for (var i = 0; i < N; i++) if (model[i]) ones.push(i);
assert_eq(bs.count, ones.length, "bitset count");
assert_eq(JSON.stringify(bs.toArray()), JSON.stringify(ones), "bitset toArray");
var it = [];
for (var x of bs) it.push(x);
assert_eq(JSON.stringify(it), JSON.stringify(ones), "bitset iterate");
// set ops vs models
var a = new BitSet(40), b = new BitSet(40);
[1, 2, 3, 20].forEach(function (i) { a.set(i); });
[3, 20, 30].forEach(function (i) { b.set(i); });
var aModel = [1, 2, 3, 20], bModel = [3, 20, 30];
// NOTE: and/or/xor mutate in place (-> this), so apply to copies in order
var ac = new BitSet(40); [1, 2, 3, 20].forEach(function (i) { ac.set(i); });
ac.and(b);
assert_eq(JSON.stringify(ac.toArray()), JSON.stringify(aModel.filter(function (x) { return bModel.indexOf(x) >= 0; })), "bitset and");
var ao = new BitSet(40); [1, 2, 3, 20].forEach(function (i) { ao.set(i); });
ao.or(b);
assert_eq(JSON.stringify(ao.toArray().slice().sort(function (x, y) { return x - y; })), JSON.stringify([1, 2, 3, 20, 30]), "bitset or");
var ax = new BitSet(40); [1, 2, 3, 20].forEach(function (i) { ax.set(i); });
ax.xor(b);
assert_eq(JSON.stringify(ax.toArray().slice().sort(function (x, y) { return x - y; })), JSON.stringify([1, 2, 30]), "bitset xor");
// auto-grow
var ag = new BitSet();
ag.set(2000);
assert_eq(ag.nextSet(0), 2000, "bitset auto-grow");
// ---- UnionFind randomized vs model
var uf = new UnionFind(50);
var parent = [];
for (var i = 0; i < 50; i++) parent.push(i);
function find(x) { while (parent[x] !== x) { parent[x] = parent[parent[x]]; x = parent[x]; } return x; }
bad = 0;
for (var step2 = 0; step2 < 500; step2++) {
  var u = rnd() % 50, v = rnd() % 50;
  if (rnd() % 2) {
    var merged = find(u) !== find(v);
    if (uf.union(u, v) !== merged) bad++;
    if (merged) parent[find(u)] = find(v);
  } else {
    if (uf.connected(u, v) !== (find(u) === find(v))) bad++;
  }
}
if (bad) { __fail++; __out("FAIL unionfind violations " + bad); } else __pass++;
var comps = {};
for (var i = 0; i < 50; i++) comps[find(i)] = 1;
assert_eq(uf.count, Object.keys(comps).length, "unionfind count");
assert_eq(uf.size, 50, "unionfind size");
// ---- Fenwick vs prefix sums
var n = 60, fw = new Fenwick(n), arr = [];
for (var i = 0; i < n; i++) arr.push(0);
bad = 0;
for (var step3 = 0; step3 < 600; step3++) {
  var i = rnd() % n, delta = (rnd() % 21) - 10;
  fw.update(i, delta); arr[i] += delta;
  var pi = rnd() % n;
  var want = 0;
  for (var j = 0; j <= pi; j++) want += arr[j];
  if (fw.prefixSum(pi) !== want) bad++;
}
if (bad) { __fail++; __out("FAIL fenwick violations " + bad); } else __pass++;
var total = 0;
for (var j = 0; j < n; j++) total += arr[j];
assert_eq(fw.prefixSum(n - 1), total, "fenwick full sum");
assert_eq(fw.size, n, "fenwick size");
var fser = Fenwick.deserialize(fw.serialize());
assert_eq(fser.prefixSum(n - 1), total, "fenwick ser rt");
// ---- SegTree: sum/min/max inclusive ranges vs naive
["sum", "min", "max"].forEach(function (opName) {
  var st = new SegTree(40, opName);
  var vals = [];
  for (var i = 0; i < 40; i++) vals.push(0);
  for (var s4 = 0; s4 < 300; s4++) {
    var i = rnd() % 40, v = (rnd() % 100) - 50;
    st.update(i, v); vals[i] = v;
  }
  var fold = opName === "sum" ? function (x, y) { return x + y; }
            : opName === "min" ? Math.min : Math.max;
  var identity = opName === "sum" ? 0 : opName === "min" ? Infinity : -Infinity;
  for (var q = 0; q < 50; q++) {
    var lo = rnd() % 40, hi = rnd() % 40;
    if (lo > hi) { var tmp = lo; lo = hi; hi = tmp; }
    var want = identity;
    for (var j = lo; j <= hi; j++) want = fold(want, vals[j]);
    var got = st.rangeQuery(lo, hi);
    if (got !== want) { bad++; __out("FAIL segtree " + opName + " [" + lo + "," + hi + "] got " + got + " want " + want); }
  }
  var st2 = SegTree.deserialize(st.serialize());
  if (st2.rangeQuery(0, 39) !== st.rangeQuery(0, 39)) bad++;
});
// ---- sketches
var bf = new BloomFilter(8192, 4);
for (var i = 0; i < 200; i++) bf.add("key" + i);
var fnCount = 0;
for (var i = 0; i < 200; i++) if (!bf.mayContain("key" + i)) fnCount++;
assert_eq(fnCount, 0, "bloom no false negatives");
assert_eq(bf.bits, 8192, "bloom bits");
assert_eq(bf.hashes, 4, "bloom hashes");
var fp = 0;
for (var i = 0; i < 500; i++) if (bf.mayContain("absent" + i)) fp++;
assert_true(fp < 120, "bloom fp rate bounded (" + fp + "/500)");
var bf2 = BloomFilter.deserialize(bf.serialize());
assert_eq(bf2.mayContain("key7"), true, "bloom ser rt");
var cms = new CountMinSketch(2048, 5);
var trueCounts = {};
for (var i = 0; i < 1000; i++) { var k = "k" + (i % 50); cms.add(k); trueCounts[k] = (trueCounts[k] || 0) + 1; }
var over = 0;
for (var k in trueCounts) { var c = cms.count(k); if (c < trueCounts[k]) over++; }
assert_eq(over, 0, "cms never underestimates");
assert_eq(cms.totalCount, 1000, "cms totalCount");
assert_eq(cms.width, 2048, "cms width");
assert_eq(cms.depth, 5, "cms depth");
var cms2 = new CountMinSketch(2048, 5);
cms2.add("k0", 10);
cms.merge(cms2);
assert_eq(cms.count("k0") >= trueCounts["k0"] + 10, true, "cms merge adds");
var hll = new HyperLogLog();
for (var i = 0; i < 10000; i++) hll.add("u" + i);
var est = hll.count();
var relerr = Math.abs(est - 10000) / 10000;
assert_true(relerr < 0.05, "hll 10k distinct within 5% (err " + relerr + ")");
var hll2 = new HyperLogLog();
for (var i = 0; i < 10000; i++) hll2.add("u" + i);
hll.merge(hll2);
var est2 = hll.count();
assert_true(Math.abs(est2 - 10000) / 10000 < 0.05, "hll merge keeps cardinality (" + est2 + ")");
assert_eq(hll.precision > 0 && hll.precision <= 16, true, "hll precision sane");
assert_eq(hll.registers, Math.pow(2, hll.precision), "hll registers = 2^p");
// ---- RangeSet: coalescing + complement + measure
var rs = new RangeSet();
rs.add(1, 3); rs.add(4, 6); rs.add(2, 5);
assert_eq(JSON.stringify(rs.ranges()), "[[1,6]]", "rangeset coalesce");
assert_eq(rs.size, 1, "rangeset size");
assert_eq(rs.measure, 5, "rangeset measure");
assert_eq(rs.contains(5), true, "rangeset contains");
assert_eq(rs.contains(6), false, "rangeset end exclusive");
assert_eq(rs.encloses(2, 5), true, "rangeset encloses");
assert_eq(rs.intersects(6, 9), false, "rangeset intersects");
assert_eq(rs.intersects(5, 9), true, "rangeset intersects at last");
assert_eq(JSON.stringify(rs.complement(0, 10)), "[[0,1],[6,10]]", "rangeset complement");
rs.remove(2, 4);
assert_eq(JSON.stringify(rs.ranges()), "[[1,2],[4,6]]", "rangeset remove splits");
rs.clear();
assert_eq(rs.size, 0, "rangeset clear");
// ---- RangeMap
var rm = new RangeMap();
rm.put(0, 10, "lo");
assert_eq(rm.get(5), "lo", "rangemap get mid");
assert_eq(rm.get(10), undefined, "rangemap end exclusive");
assert_eq(rm.get(-1), undefined, "rangemap before");
assert_eq(rm.size, 1, "rangemap size");
rm.remove(0, 5);
assert_eq(rm.get(7), "lo", "rangemap remove splits");
// ---- IntervalTree
var it2 = new IntervalTree();
it2.insert(0, 10, "a"); it2.insert(20, 30, "b"); it2.insert(5, 22, "c");
var ovl = it2.overlapping(9, 21);
assert_eq(ovl.length, 3, "interval overlapping count");
assert_eq(JSON.stringify(it2.at(25).length >= 1), "true", "interval at");
assert_eq(it2.size, 3, "interval size");
summary("structures_bits_prob");
""")
    return write_probe("structures", "bits_prob", "\n".join(emit))


if __name__ == "__main__":
    print(probe_graph())
    print(probe_linear())
    print(probe_sorted())
    print(probe_keyed())
    print(probe_bits_prob())
