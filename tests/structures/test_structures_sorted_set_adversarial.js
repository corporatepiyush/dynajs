// covers: dyna:structures.SortedSet
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_structures_sorted_set_adversarial");
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

__kitS.section("SortedSet.has(NaN) is false", () => {
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
    const gu = new Graph();
    throws(() => gu.addEdge(0, 1, NaN), RangeError,
           "even an unweighted graph refuses a NaN weight");
    g.addEdge(0, 1, -5);
    check(g.bellmanFord(0)[1] === -5, "a finite negative weight still works");

    const hn = new Heap();
    hn.push(3); hn.push(NaN); hn.push(1);
    check(hn.pop() === 1, "natural-mode Heap accepts NaN without corrupting order");
    const h0 = new Heap();
    throws(() => h0.push(), TypeError, "push() with no argument throws");
    h0.push(7); h0.push(2);
    check(h0.pop() === 2 && h0.pop() === 7,
          "and the same heap stays usable afterwards");
    const mh = new MinMaxHeap();
    throws(() => mh.push(NaN), RangeError, "MinMaxHeap.push(NaN)");
});
__kitS.end();

if (fails === 0) print("test_structures_adversarial: all " + n + " checks passed");
else print("test_structures_adversarial: " + fails + " FAILED of " + n);
