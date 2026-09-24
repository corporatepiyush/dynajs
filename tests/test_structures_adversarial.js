/* test_structures_adversarial.js -- hostile arguments, boundary values and
 * reentrancy for dyna:structures: the cases a happy-path test never produces.
 *
 * Four families, each named by the failure it exists to catch:
 *   1. allocation bombs: an index or capacity that coerces to a huge uint32
 *      must throw in bounded time, never grow a container toward it
 *      (BitSet.set(-1) was a measured 512 MB);
 *   2. non-finite values: NaN/+-Infinity where the structure's invariant
 *      needs a real order -- either refused (keys) or finite-only (weights);
 *   3. user code running mid-operation: a mutating or NaN-returning aStar
 *      heuristic must be rejected, not produce a wrong path;
 *   4. boundary and edge states: empty structures, exact N-1/N/N+1 indexes,
 *      byte-string keys with embedded NULs, fractional counts.
 */
import {
    BitSet, UnionFind, Deque, Fenwick, RingBuffer, SegTree, BloomFilter,
    Trie, LRU, SortedSet, SortedMap, Heap,
    Multiset, Multimap, Table, RangeSet, RangeMap, IntervalTree,
    MinMaxHeap, CountMinSketch, HyperLogLog, Graph,
} from "dyna:structures";

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

/* ==================================================================== *
 *  1. Allocation bombs
 * ==================================================================== */
{
    const b = new BitSet();
    throws(() => b.set(-1), RangeError, "BitSet.set(-1)");
    throws(() => b.set(2 ** 32 - 1), RangeError, "BitSet.set(2^32-1)");
    throws(() => b.set(2 ** 30), RangeError, "BitSet.set(2^30) -- past the ctor ceiling");
    throws(() => b.flip(-1), RangeError, "BitSet.flip(-1)");
    throws(() => b.flip(2 ** 30), RangeError, "BitSet.flip(2^30)");
    b.set(2 ** 30 - 1);
    check(b.get(2 ** 30 - 1) === true, "set(2^30-1) -- the largest legal bit -- works");
    check(b.get(-1) === false, "get(-1) is false, not a 4-billion-index probe");
    b.clear(-1);                                 /* clear never grows: must not throw */

    for (const [name, make] of [
        ["UnionFind", () => new UnionFind(1e9)],
        ["UnionFind", () => new UnionFind(-1)],
        ["UnionFind", () => new UnionFind(0.5)],
        ["UnionFind", () => new UnionFind(NaN)],
        ["UnionFind", () => new UnionFind(2 ** 53)],
        ["Fenwick", () => new Fenwick(1e9)],
        ["Fenwick", () => new Fenwick(-1)],
        ["Fenwick", () => new Fenwick(0.5)],
        ["Fenwick", () => new Fenwick(NaN)],
        ["SegTree", () => new SegTree(1e9)],
        ["SegTree", () => new SegTree(-1)],
        ["SegTree", () => new SegTree(0.5)],
        ["SegTree", () => new SegTree(NaN, "min")],
        ["RingBuffer", () => new RingBuffer(1e9)],
        ["RingBuffer", () => new RingBuffer(-1)],
        ["RingBuffer", () => new RingBuffer(0.5)],
        ["LRU", () => new LRU(3e9)],
        ["LRU", () => new LRU(-1)],
        ["LRU", () => new LRU(0.5)],
        ["LRU", () => new LRU(NaN)],
        ["BloomFilter", () => new BloomFilter(2 ** 32, 3)],
        ["BloomFilter", () => new BloomFilter(-1, 3)],
        ["BloomFilter", () => new BloomFilter(0.5, 3)],
    ]) {
        throws(make, RangeError, name + " refuses the bomb capacity");
    }
    /* Deque takes no capacity argument at all: passing a bomb must not be
     * read as one, so the container stays empty. */
    check(new Deque(1e9).length === 0, "Deque ignores a stray capacity argument");
    /* The same capacities as legit small integers still construct. */
    check(new UnionFind(8).size === 8, "UnionFind(8) constructs");
    check(new Fenwick(8).size === 8, "Fenwick(8) constructs");
    check(new SegTree(8, "min").size === 8, "SegTree(8) constructs");
    check(new LRU(8).capacity === 8, "LRU(8) constructs");
    check(new BloomFilter(64, 2).bits === 64, "BloomFilter(64, 2) constructs");
}

/* ==================================================================== *
 *  1b. Wrapped-index aliases: ToUint32(2^32+k) is k BY SPEC, so a huge
 *  index used to silently hit a small real one past every guard.
 * ==================================================================== */
{
    const bs = new BitSet();
    throws(() => bs.set(2 ** 32 + 3), RangeError, "BitSet.set(2^32+3) refuses instead of aliasing bit 3");
    check(bs.get(3) === false, "and bit 3 was left untouched");
    throws(() => bs.get(2 ** 32 + 3), RangeError, "BitSet.get(2^32+3) throws");
    throws(() => bs.flip(2 ** 32 + 3), RangeError, "BitSet.flip(2^32+3) throws");

    const uf = new UnionFind(8);
    throws(() => uf.find(2 ** 32 + 3), RangeError, "UnionFind.find(2^32+3) throws");
    throws(() => uf.union(2 ** 32 + 3, 0), RangeError, "UnionFind.union(2^32+3, 0) throws");
    throws(() => uf.connected(2 ** 32 + 3, 0), RangeError, "UnionFind.connected(2^32+3, 0) throws");

    const d = new Deque();
    d.pushBack(1);
    throws(() => d.get(2 ** 32 + 3), RangeError, "Deque.get(2^32+3) throws");

    const rb = new RingBuffer(4);
    throws(() => rb.get(2 ** 32 + 3), RangeError, "RingBuffer.get(2^32+3) throws");
    throws(() => new RingBuffer(2 ** 32 + 16), RangeError,
           "RingBuffer(2^32+16) refuses instead of building a 16-slot buffer");
    throws(() => new RingBuffer(1.5), RangeError,
           "RingBuffer(1.5) refuses instead of silently truncating to 1");

    const f = new Fenwick(8);
    throws(() => f.update(2 ** 32 + 3, 1), RangeError, "Fenwick.update(2^32+3) throws");
    check(f.prefixSum(3) === 0, "and index 3 was left untouched");
    throws(() => f.prefixSum(2 ** 32 + 3), RangeError, "Fenwick.prefixSum(2^32+3) throws");

    const sg = new SegTree(8, "sum");
    throws(() => sg.update(2 ** 32 + 3, 1), RangeError, "SegTree.update(2^32+3) throws");
    throws(() => sg.rangeQuery(2 ** 32, 2), RangeError, "SegTree.rangeQuery(2^32, 2) throws");

    const g = new Graph();
    g.addNode(); g.addNode();
    throws(() => g.addEdge(2 ** 32 + 1, 0), RangeError,
           "addEdge(2^32+1, 0) refuses instead of aliasing node 1");
    check(g.edgeCount === 0, "and no edge landed");
    throws(() => g.hasEdge(2 ** 32 + 1, 0), RangeError, "hasEdge(2^32+1, 0) throws");
    throws(() => g.neighbors(2 ** 32 + 1), RangeError, "neighbors(2^32+1) throws");
    throws(() => g.bfs(2 ** 32 + 1), RangeError, "bfs(2^32+1) throws");
    throws(() => g.dijkstra(2 ** 32 + 1), RangeError, "dijkstra(2^32+1) throws");
    throws(() => g.bellmanFord(2 ** 32 + 1), RangeError, "bellmanFord(2^32+1) throws");
    throws(() => g.aStar(2 ** 32 + 1, 0, () => 0), RangeError, "aStar(2^32+1, ...) throws");
}

/* ==================================================================== *
 *  2. Non-finite values
 * ==================================================================== */
{
    const ss = new SortedSet();
    throws(() => ss.add(NaN), RangeError, "SortedSet.add(NaN)");
    check(ss.has(NaN) === false, "SortedSet.has(NaN) is false");
    check(ss.delete(NaN) === false, "SortedSet.delete(NaN) is false");
    ss.add(Infinity); ss.add(-Infinity); ss.add(0);
    check(J(ss.toArray()) === J([-Infinity, 0, Infinity]),
          "+-Infinity are legal SortedSet keys and order correctly");
    check(ss.floor(-1e308) === -Infinity, "floor below everything is -Infinity");

    const sm = new SortedMap();
    throws(() => sm.set(NaN, 1), RangeError, "SortedMap.set(NaN)");
    check(sm.get(NaN) === undefined, "SortedMap.get(NaN) is undefined");
    sm.set(-Infinity, "lo").set(Infinity, "hi");
    check(sm.firstKey() === -Infinity && sm.lastKey() === Infinity,
          "+-Infinity are legal SortedMap keys");

    const g = new Graph({ directed: true, weighted: true });
    throws(() => g.addEdge(0, 1, NaN), RangeError, "addEdge weight NaN");
    throws(() => g.addEdge(0, 1, Infinity), RangeError, "addEdge weight +Infinity");
    throws(() => g.addEdge(0, 1, -Infinity), RangeError, "addEdge weight -Infinity");
    const gu = new Graph();                          /* unweighted: weight is ignored */
    throws(() => gu.addEdge(0, 1, NaN), RangeError,
           "even an unweighted graph refuses a NaN weight");
    g.addEdge(0, 1, -5);                             /* negative is legal for BF */
    check(g.bellmanFord(0)[1] === -5, "a finite negative weight still works");

    /* a heap WITHOUT a comparator is strict about types but NaN is a number:
     * it compares equal to itself, like the comparator path */
    const hn = new Heap();
    hn.push(3); hn.push(NaN); hn.push(1);
    check(hn.pop() === 1, "natural-mode Heap accepts NaN without corrupting order");
    /* push() with NO argument must not poison the heap: the old code stored
     * the stale slot and then threw from the sift, leaving it behind. */
    const h0 = new Heap();
    throws(() => h0.push(), TypeError, "push() with no argument throws");
    h0.push(7); h0.push(2);
    check(h0.pop() === 2 && h0.pop() === 7,
          "and the same heap stays usable afterwards");
    const mh = new MinMaxHeap();
    throws(() => mh.push(NaN), RangeError, "MinMaxHeap.push(NaN)");
}

/* ==================================================================== *
 *  3. User code mid-operation (aStar)
 * ==================================================================== */
{
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
    /* An inadmissible (overestimating) heuristic must terminate and return a
     * finite result -- suboptimality is the caller's accepted cost. */
    const r = g.aStar(0, 4, (i) => i === 2 ? 100 : 0);
    check(Number.isFinite(r.dist), "aStar with an inadmissible heuristic terminates");
    /* The guard rejects the QUERY but cannot un-mutate: the heuristic's
     * addEdge/addNode landed before the check fired. Pin the honest contract:
     * the graph carries the mutations and stays usable. */
    check(g.edgeCount === 7 && g.nodeCount === 6,
          "the refused heuristics' mutations landed (edgeCount " + g.edgeCount +
          ", nodeCount " + g.nodeCount + ")");
    check(Number.isFinite(g.aStar(0, 4, () => 0).dist),
          "and aStar still runs on the mutated graph");
}

/* ==================================================================== *
 *  4. Empty graphs and self-loops
 * ==================================================================== */
{
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
    neg.addEdge(0, 0, -1);                          /* a negative self-loop IS a cycle */
    throws(() => neg.bellmanFord(0), RangeError,
           "bellmanFord detects a negative cycle via a self-loop");

    /* Floyd used to SILENTLY return a matrix with negative diagonals for a
     * negative cycle. */
    const fg = new Graph({ directed: true, weighted: true });
    fg.addEdge(0, 1, 1).addEdge(1, 0, -2);
    throws(() => fg.floydWarshall(), RangeError, "floydWarshall throws on a negative cycle");

    /* A negative edge in a component the search cannot reach: Dijkstra and
     * aStar now refuse up front instead of succeeding silently. */
    const ng = new Graph({ directed: true, weighted: true });
    ng.addEdge(0, 1, 2);                            /* reachable, clean */
    ng.addEdge(2, 3, -5);                           /* far component, negative */
    throws(() => ng.dijkstra(0), RangeError, "dijkstra refuses an unreachable negative edge");
    throws(() => ng.aStar(0, 1, () => 0), RangeError, "aStar refuses an unreachable negative edge");

    /* A chain of huge negative weights underflows path sums but has NO
     * cycle: bellmanFord used to report a false "negative cycle". */
    const under = new Graph({ directed: true, weighted: true });
    under.addEdge(0, 1, -1e308).addEdge(1, 2, -1e308);
    const bf = under.bellmanFord(0);
    check(bf[2] === -Infinity,
          "bellmanFord reports the true -Infinity distance instead of a false cycle");
}

/* ==================================================================== *
 *  5. Boundary indexes: exact N-1 / N / N+1
 * ==================================================================== */
{
    const d = new Deque();
    d.pushBack(10); d.pushBack(20); d.pushBack(30);      /* N = 3 */
    check(d.get(0) === 10 && d.get(2) === 30, "Deque.get at N-1");
    check(d.get(3) === undefined, "Deque.get at N is undefined");
    check(d.get(-1) === undefined, "Deque.get(-1) is undefined");
    check(d.get(1e9) === undefined, "Deque.get(1e9) is undefined");

    const r = new RingBuffer(2);
    r.push(1); r.push(2); r.push(3);                     /* overwrite: count stays 2 */
    check(r.get(0) === 2 && r.get(1) === 3, "RingBuffer wraps at capacity");
    check(r.get(2) === undefined, "RingBuffer.get at N is undefined");

    const f = new Fenwick(4);
    f.update(3, 5);
    check(f.prefixSum(3) === 5 && f.rangeQuery(0, 3) === 5, "Fenwick last index");
    throws(() => f.update(4, 1), RangeError, "Fenwick.update at N throws");
    throws(() => f.update(-1, 1), RangeError, "Fenwick.update(-1) throws");
    const s = new SegTree(4, "sum");
    throws(() => s.update(4, 1), RangeError, "SegTree.update at N throws");
    throws(() => s.rangeQuery(-1, 2), RangeError, "SegTree.rangeQuery(-1, ...) throws");

    const u = new UnionFind(3);
    throws(() => u.find(3), RangeError, "UnionFind.find at N throws");
    throws(() => u.union(0, 3), RangeError, "UnionFind.union past N throws");
}

/* ==================================================================== *
 *  6. Byte-string keys: embedded NULs are ordinary bytes
 * ==================================================================== */
{
    const ms = new Multiset();
    ms.add("a\u0000b", 3);
    check(ms.count("a\u0000b") === 3, "Multiset counts a NUL-containing key");
    check(ms.count("a") === 0, "and does not truncate it at the NUL");

    const t = new Table();
    t.put("r\u0000x", "c", 7);
    check(t.get("r\u0000x", "c") === 7, "Table stores a NUL-containing row key");
    check(t.get("r", "c") === undefined, "and does not truncate it");

    const mm = new Multimap();
    mm.put("k\u0000z", 1);
    check(mm.count("k\u0000z") === 1 && mm.count("k") === 0,
          "Multimap distinguishes a NUL-containing key");

    const lru = new LRU(4);
    lru.set("x\u0000y", 9);
    check(lru.get("x\u0000y") === 9 && lru.get("x") === undefined,
          "LRU keys are byte strings too");

    const tr = new Trie();
    tr.insert("p\u0000q");
    check(tr.has("p\u0000q") === true && tr.has("p") === false,
          "Trie keys are byte strings too");
}

/* ==================================================================== *
 *  7. Fractional and extreme counts
 * ==================================================================== */
{
    const ms = new Multiset();
    check(ms.add("k", 1.5) === 1, "add('k', 1.5) coerces per ToInt64: count 1");
    throws(() => ms.add("k", -1), RangeError, "add('k', -1)");
    ms.remove("k", 1);
    check(ms.count("k") === 0, "remove('k', 1) drains the coerced count");

    const cms = new CountMinSketch(512, 4);
    cms.add("big", 2 ** 53);
    check(cms.count("big") >= 2 ** 53, "CMS counts 2^53 exactly (never under-counts)");
    throws(() => cms.add("x", -1), RangeError, "CMS refuses a negative count");
    check(cms.add("x", 1.5) === cms, "CMS truncates a fractional count per ToInt64");
    check(cms.count("x") >= 1, "and the truncated count landed");
    /* Saturation: four adds of 2^62 would wrap a u64 counter to zero (and the
     * old int64 return cast reported a NEGATIVE count). A count must never
     * shrink as more is added. */
    const sat = new CountMinSketch(64, 2);
    for (let i = 0; i < 4; i++) sat.add("big", 2 ** 62);
    check(sat.count("big") > 2 ** 62, "CMS count saturates instead of wrapping (got " + sat.count("big") + ")");
    check(sat.totalCount > 2 ** 62, "CMS totalCount stays positive past 2^63 (got " + sat.totalCount + ")");
    throws(() => new CountMinSketch(2 ** 24 + 1, 1), RangeError,
           "CountMinSketch past the 2^24-counter ceiling is refused");
    throws(() => new CountMinSketch(2 ** 28, 1), RangeError,
           "the old 2^28 ceiling (2 GiB) is gone");

    const ms2 = new Multiset();
    /* 2^63-1024 is the largest integer a JS float64 can still hand to
     * ToInt64 exactly; 2**63-1 rounds UP to 2^63 and the coercion refuses. */
    ms2.add("huge", 2 ** 63 - 1024);
    ms2.add("huge", 2 ** 63 - 1024);
    check(ms2.has("huge") === true, "Multiset.has answers for counts >= 2^63 (it used to THROW)");
    check(ms2.count("huge") > 2 ** 62, "Multiset.count is positive past 2^63 (got " + ms2.count("huge") + ")");
    ms2.add("huge", 2 ** 63 - 1024);            /* would wrap past 2^64 */
    check(ms2.count("huge") > 2 ** 62 && ms2.totalSize > 2 ** 62,
          "Multiset counts saturate instead of wrapping to a small number");
    throws(() => ms2.setCount("k"), TypeError, "setCount(key) with no count refuses");
    const mm3 = new Multimap();
    mm3.put("k", 1);
    throws(() => mm3.removeAt("k"), TypeError, "removeAt(key) with no index refuses");
    check(mm3.count("k") === 1, "and nothing was removed");

    const h = new HyperLogLog(10);
    for (let i = 0; i < 100; i++) h.add("u" + i);
    const h2 = new HyperLogLog(10);
    for (let i = 50; i < 150; i++) h2.add("u" + i);
    const once = HyperLogLog.deserialize(h.serialize());
    once.merge(h2);
    once.merge(h2);                                  /* merging the same sketch twice */
    const expected = HyperLogLog.deserialize(h.serialize());
    expected.merge(h2);
    check(once.count() === expected.count(),
          "merging the same HLL twice is idempotent, got " + once.count() +
          " vs " + expected.count());
    throws(() => h.merge(new HyperLogLog(11)), TypeError,
           "HLL merge across precisions is refused");
}

/* ==================================================================== *
 *  8. Order and eviction invariants under mutation
 * ==================================================================== */
{
    const mm = new Multimap();
    mm.put("x", 1).put("x", 2).put("x", 3);
    check(J(mm.get("x")) === J([1, 2, 3]), "Multimap.get returns insertion order");
    mm.removeAt("x", 1);
    check(J(mm.get("x")) === J([1, 3]), "removeAt(1) leaves the order [1, 3]");
    check(J(mm.keys()) === J(["x"]), "and the key set is intact");

    const tr = new Trie();
    tr.insert("ab").insert("abc").insert("abd");
    check(tr.delete("ab") === true, "deleting a prefix that is a terminal");
    check(tr.has("abc") === true && tr.has("abd") === true,
          "keeps the words that had it as a prefix");
    check(tr.has("ab") === false, "and the prefix itself is gone");

    const L = new LRU(2);
    L.set("a", 1); L.set("b", 2);
    const stats0 = L.stats;
    L.purgeExpired();                                /* nothing expired: a no-op */
    check(J(L.stats) === J(stats0), "purgeExpired with nothing expired changes no counter");
    check(L.get("a") === 1 && L.get("b") === 2, "and evicts nothing");

    const L2 = new LRU(2);
    L2.set("a", 1); L2.set("b", 2); L2.set("c", 3);
    const s2 = L2.stats;
    check(s2.evictions === 1 && s2.size === 2, "evictions and size counters after an eviction");
    check(s2.hits === 0 && s2.misses === 0 && s2.expired === 0,
          "hits/misses/expired all start at zero");
}

/* ==================================================================== *
 *  9. Ranges: infinity spans, splits, no-op removals
 * ==================================================================== */
{
    const rs = new RangeSet();
    rs.add(-Infinity, Infinity);
    check(rs.measure === Infinity, "a full-line RangeSet measures Infinity");
    check(rs.contains(0) && rs.contains(-1e308) && rs.contains(1e308), "and contains everything");
    rs.remove(0, 1);
    check(rs.contains(0.5) === false, "remove carves a hole in it");
    check(rs.measure === Infinity, "and the measure stays infinite");

    const rm = new RangeMap();
    rm.put(0, 10, "a").put(20, 30, "a");
    rm.remove(10, 20);                               /* the removed span held no value */
    check(rm.size === 2, "removing an unset span changes nothing");
    check(rm.get(5) === "a" && rm.get(25) === "a", "both survivors keep their values");
    rm.put(0, 10, "b").put(20, 30, "b");
    rm.remove(5, 25);
    check(rm.size === 2, "a remove across both spans trims them to the survivors");
    check(rm.get(2) === "b" && rm.get(28) === "b", "the trimmed spans keep their values");
    check(rm.get(15) === undefined, "and the middle is gone");

    /* remove() must agree with a naive model: the bisect change to rset
     * remove is exercised here across a full window sweep. */
    const model = new Set();
    const rs2 = new RangeSet();
    for (let i = 0; i < 300; i++) { rs2.add(i * 2, i * 2 + 1); model.add(i); }
    for (let w = 0; w < 300; w += 7) {
        rs2.remove(w * 2, (w + 3) * 2);
        for (let i = w; i < w + 3; i++) model.delete(i);
    }
    let agree = true;
    for (let i = 0; i < 300; i++)
        if (rs2.contains(i * 2 + 0.5) !== model.has(i)) { agree = false; break; }
    check(agree, "RangeSet.remove agrees with a naive model over a sweep");
    check(rs2.size === model.size, "and the span count matches ("
          + rs2.size + " vs " + model.size + ")");

    /* rmap put/remove round trip against a dense model */
    const rmap = new RangeMap();
    const dense = new Map();
    for (let i = 0; i < 200; i++) {
        rmap.put(i * 10, i * 10 + 8, "v" + (i % 5));
        for (let x = i * 10; x < i * 10 + 8; x++) dense.set(x, "v" + (i % 5));
    }
    rmap.remove(500, 1500);
    for (let x = 500; x < 1500; x++) dense.delete(x);
    let ok = true;
    for (let x = 0; x < 2000; x++) {
        const got = rmap.get(x), want = dense.get(x);
        if (got !== want) { ok = false; break; }
    }
    check(ok, "RangeMap agrees with a dense model after a wide remove");
}

/* ==================================================================== *
 *  10. IntervalTree: the burst-rebuild path must not change answers
 * ==================================================================== */
{
    const t = new IntervalTree();
    for (let i = 0; i < 5000; i++) t.insert(i * 10, i * 10 + 25, i);
    t.at(0);                                          /* index built */
    for (let i = 0; i < 600; i++) t.insert(1e9 + i, 1e9 + i + 5, "p" + i);
    /* first query scans the pending tail; the second triggers the burst
     * rebuild -- the two must agree, and both must agree with a tree built
     * from the same intervals in one go. */
    const a = J(t.at(50).map(x => x[2]).sort());
    const b = J(t.at(50).map(x => x[2]).sort());
    check(a === b, "itree pending-scan and post-rebuild queries agree");
    const fresh = new IntervalTree();
    for (let i = 0; i < 5000; i++) fresh.insert(i * 10, i * 10 + 25, i);
    for (let i = 0; i < 600; i++) fresh.insert(1e9 + i, 1e9 + i + 5, "p" + i);
    fresh.at(0);
    const c = J(fresh.at(50).map(x => x[2]).sort());
    check(a === c, "and both agree with a freshly built tree");
    /* Intervals are CLOSED: hi === x counts. p295's hi is exactly 1e9+300, so
     * the point query returns six entries, not one. */
    const p = t.at(1e9 + 300).map(x => x[2]);
    check(J(p) === J(["p295", "p296", "p297", "p298", "p299", "p300"]),
          "pending intervals answer point queries with closed-boundary semantics");
    check(J(t.at(1e9 + 610)) === J([]), "a point past the last hi matches nothing");
    check(J(t.overlapping(10, 5)) === J([]),
          "an inverted overlapping() range names nothing");
}

/* ==================================================================== *
 *  11. Table row/column after deletes
 * ==================================================================== */
{
    const t = new Table();
    t.put("r", "c1", 1).put("r", "c2", 2);
    check(J(t.row("r")) === J([["c1", 1], ["c2", 2]]), "row() collects a row as pairs");
    t.delete("r", "c1");
    check(J(t.row("r")) === J([["c2", 2]]), "row() follows a delete");
    check(J(t.column("c1")) === J([]), "column() of a fully deleted column is empty");
    check(J(t.row("absent")) === J([]), "row() of an absent key is an empty array");
    t.put("r2", "c1", 5);
    check(J(t.column("c1")) === J([["r2", 5]]), "column() pairs a surviving cell with its row");
}

/* ==================================================================== *
 *  12. Mutation during iteration is well defined (snapshot semantics)
 * ==================================================================== */
{
    const d = new Deque();
    d.pushBack(1); d.pushBack(2);
    let seen = [];
    for (const v of d) { seen.push(v); d.pushBack(v * 10); }
    check(J(seen) === J([1, 2]), "mutating a Deque mid-iteration does not extend the loop");
    check(d.length === 4, "and the mutations landed");

    const ss = new SortedSet();
    ss.add(2); ss.add(1);
    let got = [];
    for (const v of ss) { got.push(v); if (v === 1) ss.add(3); }
    check(J(got) === J([1, 2]), "mutating a SortedSet mid-iteration does not extend the loop");
}

if (fails === 0) print("test_structures_adversarial: all " + n + " checks passed");
else print("test_structures_adversarial: " + fails + " FAILED of " + n);
