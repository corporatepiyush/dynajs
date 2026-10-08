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

{
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
}

{
    eqJSON([...new BitSet()], [], "empty BitSet");
    eqJSON([...new Deque()], [], "empty Deque");
    eqJSON([...new RingBuffer(4)], [], "empty RingBuffer");
    eqJSON([...new SortedSet()], [], "empty SortedSet");
    eqJSON([...new Trie()], [], "empty Trie");
    eqJSON([...new List()], [], "empty List");
    eqJSON([...new LRU(4)], [], "empty LRU");
}

{
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
}

{
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
}

{
    const noIter = [
        ["BloomFilter", new BloomFilter(64, 3)],
        ["Heap",        new Heap((a, b) => a - b)],
        ["UnionFind",   new UnionFind(4)],
        ["Fenwick",     new Fenwick(4)],
        ["SegTree",     new SegTree(4)],
        ["SortedMap",   new SortedMap()],
    ];
    for (const [name, c] of noIter) {
        assert(c[Symbol.iterator] === undefined,
               name + " deliberately has no [Symbol.iterator] (see dyna-structures.c)");
        n++;
        let threw = false;
        try { [...c]; } catch (e) { threw = e instanceof TypeError; }
        if (!threw) throw new Error(name + ": spreading a non-iterable must throw TypeError");
    }
}

{
    const d = build.Deque();
    for (let i = 0; i < 2000; i++) {
        const got = [...d];
        if (got.length !== 4) throw new Error("Deque iteration drifted at round " + i);
    }
    n++;
    const t = build.Trie();
    for (let i = 0; i < 2000; i++)
        if ([...t].length !== 4) throw new Error("Trie iteration drifted at round " + i);
    n++;
}

{
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
}

print("test_structures_iter: all " + n + " tests passed");
