// covers: dyna:structures.Multimap
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_structures_multimap_adversarial");
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

__kitS.section("Multimap.get returns insertion order", () => {
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
    L.purgeExpired();
    check(J(L.stats) === J(stats0), "purgeExpired with nothing expired changes no counter");
    check(L.get("a") === 1 && L.get("b") === 2, "and evicts nothing");

    const L2 = new LRU(2);
    L2.set("a", 1); L2.set("b", 2); L2.set("c", 3);
    const s2 = L2.stats;
    check(s2.evictions === 1 && s2.size === 2, "evictions and size counters after an eviction");
    check(s2.hits === 0 && s2.misses === 0 && s2.expired === 0,
          "hits/misses/expired all start at zero");
});
__kitS.end();

if (fails === 0) print("test_structures_adversarial: all " + n + " checks passed");
else print("test_structures_adversarial: " + fails + " FAILED of " + n);
