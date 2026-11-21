// covers: dyna:structures.SortedMap
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_structures_sorted_map_iter");
import {
    BitSet, Deque, RingBuffer, SortedSet, Trie, List,
    BloomFilter, Heap, UnionFind, Fenwick, SegTree, LRU, SortedMap,
} from "dyna:structures";

let n = 0;
function assert(c, m) { n++; if (!c) throw new Error("assertion failed: " + m); }
function eqJSON(got, want, m) {
    n++;
    const a = JSON.stringify(got), b = JSON.stringify(want);
    if (a !== b) throw new Error("assertion failed: " + m + "\n  got:  " + a + "\n  want: " + b);
}

const build = {
    BitSet:     () => { const b = new BitSet(); [1, 5, 9, 64, 65].forEach(i => b.set(i)); return b; },
    Deque:      () => { const d = new Deque(); [1, 2, 3, 4].forEach(v => d.pushBack(v)); return d; },
    RingBuffer: () => { const r = new RingBuffer(4); [1, 2, 3, 4].forEach(v => r.push(v)); return r; },
    SortedSet:  () => { const s = new SortedSet(); [5, 1, 3, 2, 4].forEach(v => s.add(v)); return s; },
    Trie:       () => { const t = new Trie(); ["ab", "ac", "b", ""].forEach(k => t.insert(k)); return t; },
    List:       () => { const l = new List(); [1, 2, 3, 4].forEach(v => l.pushBack(v)); return l; },
    LRU:        () => { const l = new LRU(4); [["a", 1], ["b", 2], ["c", 3], ["d", 4]].forEach(([k, v]) => l.put(k, v)); return l; },
};
const EXPECT = {
    BitSet:     [1, 5, 9, 64, 65],
    Deque:      [1, 2, 3, 4],
    RingBuffer: [1, 2, 3, 4],
    SortedSet:  [1, 2, 3, 4, 5],
    List:       [1, 2, 3, 4],
    LRU:        [["d", 4], ["c", 3], ["b", 2], ["a", 1]],
}

__kitS.section("LRU entries() is MRU-first by insertion recency", () => {
    const l = new LRU(4);
    ["a", "b", "c", "d"].forEach((k, i) => l.put(k, i + 1));
    eqJSON(l.entries(), [["d", 4], ["c", 3], ["b", 2], ["a", 1]],
           "LRU entries() is MRU-first by insertion recency");
    l.get("b");
    eqJSON(l.entries(), [["b", 2], ["d", 4], ["c", 3], ["a", 1]],
           "get() moves a pair to the front");
    l.has("a");
    eqJSON(l.entries(), [["b", 2], ["d", 4], ["c", 3], ["a", 1]],
           "has() leaves the recency order alone");
    l.put("b", 20);
    eqJSON(l.entries()[0], ["b", 20], "put() on an existing key restamps recency");
    const snap = l.entries();
    l.put("e", 5);
    eqJSON(snap.length, 4, "entries() is a snapshot, not a live view");
    eqJSON(snap[3], ["a", 1], "and the evicted pair stays in the snapshot");

    const m = new SortedMap();
    [5, 1, 3, 2, 4].forEach(k => m.set(k, "v" + k));
    const pairs = m.entries();
    eqJSON(pairs, [[1, "v1"], [2, "v2"], [3, "v3"], [4, "v4"], [5, "v5"]],
           "SortedMap entries() is ascending by key");
    eqJSON(pairs.map(p => p[0]), m.keys(),
           "SortedMap entries() keys match keys() exactly");
    eqJSON(new SortedMap().entries(), [], "empty SortedMap entries()");
});
__kitS.end();

print("test_structures_iter: all " + n + " tests passed");
