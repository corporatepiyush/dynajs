// covers: dyna:structures.SortedSet
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_structures_sorted_set_iter");
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

__kitS.section("-> map", () => {
    const d = build.Deque();
    eqJSON([...d].values().map(x => x * 2).toArray(), [2, 4, 6, 8], "-> map");
    eqJSON([...d].values().filter(x => x % 2).toArray(), [1, 3], "-> filter");
    eqJSON([...d].values().take(2).toArray(), [1, 2], "-> take");
    eqJSON(Array.from(build.SortedSet()), [1, 2, 3, 4, 5], "Array.from works");
    const [first, second] = build.List();
    assert(first === 1 && second === 2, "destructuring works");
    eqJSON(Math.max(...build.SortedSet()), 5, "spread into a call");
    {
        const s = build.SortedSet();
        let count = 0;
        for (const _ of s) { if (++count === 2) break; }
        eqJSON(s.toArray(), [1, 2, 3, 4, 5], "break does not disturb the container");
    }
    {
        const d2 = build.Deque();
        const a = d2[Symbol.iterator](), b = d2[Symbol.iterator]();
        a.next();
        eqJSON(b.next().value, 1, "a second iterator starts from the beginning");
    }
    {
        const s = new SortedSet(); [1, 2].forEach(v => s.add(v));
        const pairs = [];
        for (const x of s) for (const y of s) pairs.push([x, y]);
        eqJSON(pairs, [[1, 1], [1, 2], [2, 1], [2, 2]], "nested iteration works");
    }
});
__kitS.end();

print("test_structures_iter: all " + n + " tests passed");
