// covers: dyna:structures.UnionFind
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_structures_union_find_adversarial");
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

__kitS.section("BitSet.set(2^30) -- past the ctor ceiling", () => {
    const b = new BitSet();
    throws(() => b.set(-1), RangeError, "BitSet.set(-1)");
    throws(() => b.set(2 ** 32 - 1), RangeError, "BitSet.set(2^32-1)");
    throws(() => b.set(2 ** 30), RangeError, "BitSet.set(2^30) -- past the ctor ceiling");
    throws(() => b.flip(-1), RangeError, "BitSet.flip(-1)");
    throws(() => b.flip(2 ** 30), RangeError, "BitSet.flip(2^30)");
    b.set(2 ** 30 - 1);
    check(b.get(2 ** 30 - 1) === true, "set(2^30-1) -- the largest legal bit -- works");
    check(b.get(-1) === false, "get(-1) is false, not a 4-billion-index probe");
    b.clear(-1);

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
    check(new Deque(1e9).length === 0, "Deque ignores a stray capacity argument");
    check(new UnionFind(8).size === 8, "UnionFind(8) constructs");
    check(new Fenwick(8).size === 8, "Fenwick(8) constructs");
    check(new SegTree(8, "min").size === 8, "SegTree(8) constructs");
    check(new LRU(8).capacity === 8, "LRU(8) constructs");
    check(new BloomFilter(64, 2).bits === 64, "BloomFilter(64, 2) constructs");
});
__kitS.end();

if (fails === 0) print("test_structures_adversarial: all " + n + " checks passed");
else print("test_structures_adversarial: " + fails + " FAILED of " + n);
