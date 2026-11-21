// covers: dyna:structures.BitSet
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_structures_bit_set_iter");
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

__kitS.section("empty BitSet", () => {
    eqJSON([...new BitSet()], [], "empty BitSet");
    eqJSON([...new Deque()], [], "empty Deque");
    eqJSON([...new RingBuffer(4)], [], "empty RingBuffer");
    eqJSON([...new SortedSet()], [], "empty SortedSet");
    eqJSON([...new Trie()], [], "empty Trie");
    eqJSON([...new List()], [], "empty List");
    eqJSON([...new LRU(4)], [], "empty LRU");
});
__kitS.end();

print("test_structures_iter: all " + n + " tests passed");
