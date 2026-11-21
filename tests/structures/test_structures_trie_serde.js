// covers: dyna:structures.Trie
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_structures_trie_serde");
import * as S from "dyna:structures";
import * as GraphMod from "dyna:graph";

let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { check(a === b, m + " -- got " + a + ", want " + b); }
function throwsOf(fn, kind, m) {
    let e = null;
    try { fn(); } catch (x) { e = x; }
    check(e !== null, m + " (expected a throw)");
    if (e && kind) check(e instanceof kind,
        m + " (wanted " + kind.name + ", got " + (e && e.constructor.name) + ")");
    return e;
}

const BUILD = {
    BiMap: () => { const x = new S.BiMap(); x.set("a", 1); return x; },
    BitSet: () => { const x = new S.BitSet(64); x.set(3); return x; },
    BloomFilter: () => { const x = new S.BloomFilter(100, 0.01); x.add("a"); return x; },
    CountMinSketch: () => { const x = new S.CountMinSketch(64, 4); x.add("a"); return x; },
    BTree: () => { const x = new S.BTree(); x.set(1, "a"); x.set(2, "b"); return x; },
    Deque: () => { const x = new S.Deque(); x.pushBack(1); return x; },
    Fenwick: () => new S.Fenwick(8),
    Graph: () => { const x = new GraphMod.Graph(); x.addNode("a"); return x; },
    Heap: () => { const x = new S.Heap((a, b) => a - b); x.push(1); return x; },
    HyperLogLog: () => { const x = new S.HyperLogLog(); x.add("a"); return x; },
    IntervalTree: () => { const x = new S.IntervalTree(); x.insert(1, 2, "v"); return x; },
    LRU: () => { const x = new S.LRU(4); x.set("a", 1); return x; },
    List: () => { const x = new S.List(); x.pushBack(1); return x; },
    MinMaxHeap: () => { const x = new S.MinMaxHeap(); x.push(1); return x; },
    Multimap: () => { const x = new S.Multimap(); x.put("a", 1); return x; },
    Multiset: () => { const x = new S.Multiset(); x.add("a"); return x; },
    RangeMap: () => { const x = new S.RangeMap(); x.put(1, 2, "v"); return x; },
    RangeSet: () => { const x = new S.RangeSet(); x.add(1, 2); return x; },
    RingBuffer: () => { const x = new S.RingBuffer(4); x.push(1); return x; },
    SegTree: () => new S.SegTree(8),
    SortedMap: () => { const x = new S.SortedMap(); x.set(1, "a"); return x; },
    SortedSet: () => { const x = new S.SortedSet(); x.add(1); return x; },
    Table: () => { const x = new S.Table(); x.put("r", "c", 1); return x; },
    Trie: () => { const x = new S.Trie(); x.insert("ab"); return x; },
    ZSet: () => { const x = new S.ZSet(); x.add("a", 1); x.add("b", 2); return x; },
    OrderedMap: () => { const x = new S.OrderedMap(); x.set("a", 1); x.set("b", [2]); return x; },
    RoaringBitmap: () => { const x = new S.RoaringBitmap([1, 70000]); x.addRange(100, 6000); return x; },
    TDigest: () => { const x = new S.TDigest(); x.add(1); x.add(5); return x; },
    TopK: () => { const x = new S.TopK(2); x.add("a"); x.add("b", 3); return x; },
    CuckooFilter: () => { const x = new S.CuckooFilter(16); x.add("a"); return x; },
    IndexedHeap: () => { const x = new S.IndexedHeap(); x.push(1, 2); x.push(3, 1); return x; },
    KDTree: () => new S.KDTree([[0, 0], [1, 2], [3, 1]]),
    ConsistentHash: () => { const x = new S.ConsistentHash({ replicas: 4 }); x.addNode("a"); x.addNode("b", 2); return x; },
    InvertedIndex: () => { const x = new S.InvertedIndex({ stopWords: ["the"] }); x.add(1, "the red fox"); x.add(2, "red dog"); return x; },
    Interner: () => { const x = new S.Interner(); x.intern("a"); x.intern("b"); return x; },
    UnionFind: () => new S.UnionFind(8),
    RBTree: () => new S.RBTree().set(2, "b").set(1, { a: 1 }),
    WBTree: () => new S.WBTree({ keys: "string" }).set("b", 2).set("a", 1),
    SkipList: () => new S.SkipList().set(-1.5, "x").set(7, [7]),
    XArray: () => { const x = new S.XArray(); x.set(3, "c").set(2n ** 63n, "big"); x.mark(3, 1); return x; },
    PatriciaTrie: () => new S.PatriciaTrie().set("app", 1).set("apple", 2),
    CSRGraph: () => new S.CSRGraph(3, [[0, 1, 2], [1, 2, 0.5]]),
    RStarTree: () => new S.RStarTree(2).insert([0, 0], [1, 1], 1).insert([2, 2], [3, 5], 2),
    SuffixArray: () => new S.SuffixArray("banana"),
};
const ARG = { Heap: (a, b) => a - b };

__kitS.section("serialize() torn off its receiver must throw, not crash", () => {
    const o = BUILD.Deque();
    const loose = o.serialize;
    throwsOf(() => loose(), TypeError,
             "serialize() torn off its receiver must throw, not crash");
    throwsOf(() => loose.call({}), TypeError, "serialize() on a plain object");
    const tr = new S.Trie(); tr.insert("ab");
    const borrowed = loose.call(tr);
    check(S.Trie.deserialize(borrowed) instanceof S.Trie,
          "serialize() borrowed by another class writes the RECEIVER's type");
    throwsOf(() => S.Deque.deserialize(borrowed), TypeError,
             "and that record is a Trie, so Deque.deserialize refuses it");
    check(loose.call(o) instanceof Uint8Array, "serialize.call(itsOwner) works");
});
__kitS.end();

if (fails === 0) print("test_structures_serde: all " + n + " checks passed");
else print("test_structures_serde: " + fails + " FAILED of " + n);
