// covers: dyna:structures.RangeSet
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_structures_range_set_adversarial");
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

__kitS.section("a full-line RangeSet measures Infinity", () => {
    const rs = new RangeSet();
    rs.add(-Infinity, Infinity);
    check(rs.measure === Infinity, "a full-line RangeSet measures Infinity");
    check(rs.contains(0) && rs.contains(-1e308) && rs.contains(1e308), "and contains everything");
    rs.remove(0, 1);
    check(rs.contains(0.5) === false, "remove carves a hole in it");
    check(rs.measure === Infinity, "and the measure stays infinite");

    const rm = new RangeMap();
    rm.put(0, 10, "a").put(20, 30, "a");
    rm.remove(10, 20);
    check(rm.size === 2, "removing an unset span changes nothing");
    check(rm.get(5) === "a" && rm.get(25) === "a", "both survivors keep their values");
    rm.put(0, 10, "b").put(20, 30, "b");
    rm.remove(5, 25);
    check(rm.size === 2, "a remove across both spans trims them to the survivors");
    check(rm.get(2) === "b" && rm.get(28) === "b", "the trimmed spans keep their values");
    check(rm.get(15) === undefined, "and the middle is gone");

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
});
__kitS.end();

if (fails === 0) print("test_structures_adversarial: all " + n + " checks passed");
else print("test_structures_adversarial: " + fails + " FAILED of " + n);
