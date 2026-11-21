// covers: dyna:structures.Trie
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_structures_trie_iter");
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

__kitS.section("has [Symbol.iterator]", () => {
    for (const name of Object.keys(build)) {
        const c = build[name]();
        assert(typeof c[Symbol.iterator] === "function", name + " has [Symbol.iterator]");
        const spread = [...c];
        if (name === "Trie") {
            eqJSON(spread.slice().sort(), ["", "ab", "ac", "b"], "Trie yields every stored key");
        } else {
            eqJSON(spread, EXPECT[name], name + " yields its documented order");
        }
        if (name === "Trie") continue;
        else if (name === "LRU") eqJSON(spread, c.entries(), name + ": iteration == entries()");
        else eqJSON(spread, c.toArray(), name + ": iteration == toArray()");
    }
});

__kitS.section("iteration did not terminate", () => {
    for (const name of Object.keys(build)) {
        const c = build[name]();
        const before = name === "Trie" ? [...c].sort() : [...c];
        const seen = [];
        let guard = 0;
        for (const v of c) {
            seen.push(v);
            if (++guard > 1000) throw new Error(name + ": iteration did not terminate");
            if (name === "BitSet") c.set(200 + guard);
            else if (name === "Deque") c.pushBack(900 + guard);
            else if (name === "RingBuffer") c.push(900 + guard);
            else if (name === "SortedSet") c.add(900 + guard);
            else if (name === "Trie") c.insert("zz" + guard);
            else if (name === "List") c.pushBack(900 + guard);
            else if (name === "LRU") c.put("zz" + guard, guard);
        }
        eqJSON(name === "Trie" ? seen.slice().sort() : seen, before,
               name + ": growth during iteration does not affect the snapshot");
    }
    {
        const d = build.Deque();
        const seen = [];
        for (const v of d) { seen.push(v); d.popFront(); d.popBack(); }
        eqJSON(seen, [1, 2, 3, 4], "Deque: draining during iteration is safe");
        eqJSON(d.toArray(), [], "and the container really did drain");
    }
    {
        const s = build.SortedSet();
        const seen = [];
        for (const v of s) { seen.push(v); s.delete(v); }
        eqJSON(seen, [1, 2, 3, 4, 5], "SortedSet: deleting during iteration is safe");
        eqJSON(s.toArray(), [], "and the deletes took effect");
    }
    {
        const t = build.Trie();
        const seen = [];
        for (const k of t) { seen.push(k); t.delete(k); }
        eqJSON(seen.slice().sort(), ["", "ab", "ac", "b"], "Trie: deleting during iteration is safe");
    }
    {
        const b = build.BitSet();
        const seen = [];
        for (const i of b) { seen.push(i); b.clear(i); }
        eqJSON(seen, [1, 5, 9, 64, 65], "BitSet: clearing during iteration is safe");
        eqJSON(b.toArray(), [], "and the clears took effect");
    }
    {
        const l = build.LRU();
        const seen = [];
        for (const [k] of l) { seen.push(k); l.delete(k); }
        eqJSON(seen, ["d", "c", "b", "a"], "LRU: deleting during iteration is safe");
        eqJSON(l.size, 0, "and the deletes took effect");
    }
});
__kitS.end();

print("test_structures_iter: all " + n + " tests passed");
