// covers: dyna:structures.Multiset
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_structures_multiset_adversarial");
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

__kitS.section("Multiset counts a NUL-containing key", () => {
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
});

__kitS.section("add('k', 1.5) coerces per ToInt64: count 1", () => {
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
    const sat = new CountMinSketch(64, 2);
    for (let i = 0; i < 4; i++) sat.add("big", 2 ** 62);
    check(sat.count("big") > 2 ** 62, "CMS count saturates instead of wrapping (got " + sat.count("big") + ")");
    check(sat.totalCount > 2 ** 62, "CMS totalCount stays positive past 2^63 (got " + sat.totalCount + ")");
    throws(() => new CountMinSketch(2 ** 24 + 1, 1), RangeError,
           "CountMinSketch past the 2^24-counter ceiling is refused");
    throws(() => new CountMinSketch(2 ** 28, 1), RangeError,
           "the old 2^28 ceiling (2 GiB) is gone");

    const ms2 = new Multiset();
    ms2.add("huge", 2 ** 63 - 1024);
    ms2.add("huge", 2 ** 63 - 1024);
    check(ms2.has("huge") === true, "Multiset.has answers for counts >= 2^63 (it used to THROW)");
    check(ms2.count("huge") > 2 ** 62, "Multiset.count is positive past 2^63 (got " + ms2.count("huge") + ")");
    ms2.add("huge", 2 ** 63 - 1024);
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
    once.merge(h2);
    const expected = HyperLogLog.deserialize(h.serialize());
    expected.merge(h2);
    check(once.count() === expected.count(),
          "merging the same HLL twice is idempotent, got " + once.count() +
          " vs " + expected.count());
    throws(() => h.merge(new HyperLogLog(11)), TypeError,
           "HLL merge across precisions is refused");
});
__kitS.end();

if (fails === 0) print("test_structures_adversarial: all " + n + " checks passed");
else print("test_structures_adversarial: " + fails + " FAILED of " + n);
