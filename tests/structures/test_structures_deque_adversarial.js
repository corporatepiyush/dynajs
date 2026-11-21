// covers: dyna:structures.Deque
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_structures_deque_adversarial");
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

__kitS.section("Deque.get at N-1", () => {
    const d = new Deque();
    d.pushBack(10); d.pushBack(20); d.pushBack(30);
    check(d.get(0) === 10 && d.get(2) === 30, "Deque.get at N-1");
    check(d.get(3) === undefined, "Deque.get at N is undefined");
    check(d.get(-1) === undefined, "Deque.get(-1) is undefined");
    check(d.get(1e9) === undefined, "Deque.get(1e9) is undefined");

    const r = new RingBuffer(2);
    r.push(1); r.push(2); r.push(3);
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
});

__kitS.section("mutating a Deque mid-iteration does not extend the loop", () => {
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
});
__kitS.end();

if (fails === 0) print("test_structures_adversarial: all " + n + " checks passed");
else print("test_structures_adversarial: " + fails + " FAILED of " + n);
