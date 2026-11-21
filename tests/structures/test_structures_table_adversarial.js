// covers: dyna:structures.Table
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_structures_table_adversarial");
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

__kitS.section("row() collects a row as pairs", () => {
    const t = new Table();
    t.put("r", "c1", 1).put("r", "c2", 2);
    check(J(t.row("r")) === J([["c1", 1], ["c2", 2]]), "row() collects a row as pairs");
    t.delete("r", "c1");
    check(J(t.row("r")) === J([["c2", 2]]), "row() follows a delete");
    check(J(t.column("c1")) === J([]), "column() of a fully deleted column is empty");
    check(J(t.row("absent")) === J([]), "row() of an absent key is an empty array");
    t.put("r2", "c1", 5);
    check(J(t.column("c1")) === J([["r2", 5]]), "column() pairs a surviving cell with its row");
});
__kitS.end();

if (fails === 0) print("test_structures_adversarial: all " + n + " checks passed");
else print("test_structures_adversarial: " + fails + " FAILED of " + n);
