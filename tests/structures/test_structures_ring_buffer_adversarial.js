// covers: dyna:structures.RingBuffer
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_structures_ring_buffer_adversarial");
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

__kitS.section("BitSet.set(2^32+3) refuses instead of aliasing bit 3", () => {
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
});
__kitS.end();

if (fails === 0) print("test_structures_adversarial: all " + n + " checks passed");
else print("test_structures_adversarial: " + fails + " FAILED of " + n);
