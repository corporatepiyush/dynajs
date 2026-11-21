// covers: dyna:structures.IntervalTree
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_structures_interval_tree_adversarial");
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

__kitS.section("itree pending-scan and post-rebuild queries agree", () => {
    const t = new IntervalTree();
    for (let i = 0; i < 5000; i++) t.insert(i * 10, i * 10 + 25, i);
    t.at(0);
    for (let i = 0; i < 600; i++) t.insert(1e9 + i, 1e9 + i + 5, "p" + i);
    const a = J(t.at(50).map(x => x[2]).sort());
    const b = J(t.at(50).map(x => x[2]).sort());
    check(a === b, "itree pending-scan and post-rebuild queries agree");
    const fresh = new IntervalTree();
    for (let i = 0; i < 5000; i++) fresh.insert(i * 10, i * 10 + 25, i);
    for (let i = 0; i < 600; i++) fresh.insert(1e9 + i, 1e9 + i + 5, "p" + i);
    fresh.at(0);
    const c = J(fresh.at(50).map(x => x[2]).sort());
    check(a === c, "and both agree with a freshly built tree");
    const p = t.at(1e9 + 300).map(x => x[2]);
    check(J(p) === J(["p295", "p296", "p297", "p298", "p299", "p300"]),
          "pending intervals answer point queries with closed-boundary semantics");
    check(J(t.at(1e9 + 610)) === J([]), "a point past the last hi matches nothing");
    check(J(t.overlapping(10, 5)) === J([]),
          "an inverted overlapping() range names nothing");
});
__kitS.end();

if (fails === 0) print("test_structures_adversarial: all " + n + " checks passed");
else print("test_structures_adversarial: " + fails + " FAILED of " + n);
