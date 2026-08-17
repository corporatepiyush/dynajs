/* test_structures_iter.js -- [Symbol.iterator] on dyna:structures.
 *
 * Two things are being pinned, and the second is the one that matters:
 *
 *   1. each iterable container yields its documented order;
 *   2. iteration is a SNAPSHOT. Every one of these delegates to a fresh array's
 *      iterator, so mutating the container inside a for...of is well defined
 *      (the loop sees the container as it was at entry) instead of walking a
 *      native pointer into something the user just reallocated. That is the
 *      whole reason for the snapshot, so it is tested for every container,
 *      including growth, shrinkage and clearing mid-loop.
 *
 * Also asserts the containers WITHOUT an iterator stay without one -- those are
 * deliberate omissions (a BloomFilter cannot enumerate its members; a Heap has
 * no non-destructive order), and a later "helpful" addition should have to
 * change a test that says so.
 *
 * Run: dynajs (CONFIG_NATIVE_MODULES=y) tests/test_structures_iter.js */

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

/* ---- builders, so each case can make a fresh container ---- */
const build = {
    BitSet:     () => { const b = new BitSet(); [1, 5, 9, 64, 65].forEach(i => b.set(i)); return b; },
    Deque:      () => { const d = new Deque(); [1, 2, 3, 4].forEach(v => d.pushBack(v)); return d; },
    RingBuffer: () => { const r = new RingBuffer(4); [1, 2, 3, 4].forEach(v => r.push(v)); return r; },
    SortedSet:  () => { const s = new SortedSet(); [5, 1, 3, 2, 4].forEach(v => s.add(v)); return s; },
    Trie:       () => { const t = new Trie(); ["ab", "ac", "b", ""].forEach(k => t.insert(k)); return t; },
    List:       () => { const l = new List(); [1, 2, 3, 4].forEach(v => l.pushBack(v)); return l; },
};
const EXPECT = {
    BitSet:     [1, 5, 9, 64, 65],
    Deque:      [1, 2, 3, 4],
    RingBuffer: [1, 2, 3, 4],
    SortedSet:  [1, 2, 3, 4, 5],
    List:       [1, 2, 3, 4],
};

/* ---- 1. presence and documented order ---- */
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
        /* the spread and the explicit array projection must agree */
        if (name !== "Trie")
            eqJSON(spread, c.toArray(), name + ": iteration == toArray()");
    }
}

/* ---- 2. empty containers iterate to nothing ---- */
{
    eqJSON([...new BitSet()], [], "empty BitSet");
    eqJSON([...new Deque()], [], "empty Deque");
    eqJSON([...new RingBuffer(4)], [], "empty RingBuffer");
    eqJSON([...new SortedSet()], [], "empty SortedSet");
    eqJSON([...new Trie()], [], "empty Trie");
    eqJSON([...new List()], [], "empty List");
}

/* ---- 3. SNAPSHOT: mutation during iteration cannot corrupt the loop ---- */
{
    /* growth mid-loop: the snapshot must not see the additions, and must not
     * run away -- an aliasing implementation would loop forever here */
    for (const name of Object.keys(build)) {
        const c = build[name]();
        const before = name === "Trie" ? [...c].sort() : [...c];
        const seen = [];
        let guard = 0;
        for (const v of c) {
            seen.push(v);
            if (++guard > 1000) throw new Error(name + ": iteration did not terminate");
            /* grow the container while iterating it */
            if (name === "BitSet") c.set(200 + guard);
            else if (name === "Deque") c.pushBack(900 + guard);
            else if (name === "RingBuffer") c.push(900 + guard);
            else if (name === "SortedSet") c.add(900 + guard);
            else if (name === "Trie") c.insert("zz" + guard);
            else if (name === "List") c.pushBack(900 + guard);
        }
        eqJSON(name === "Trie" ? seen.slice().sort() : seen, before,
               name + ": growth during iteration does not affect the snapshot");
    }
    /* shrinkage mid-loop must not read freed slots */
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
}

/* ---- 4. composes with the Iterator helper surface ---- */
{
    const d = build.Deque();
    eqJSON([...d].values().map(x => x * 2).toArray(), [2, 4, 6, 8], "-> map");
    eqJSON([...d].values().filter(x => x % 2).toArray(), [1, 3], "-> filter");
    eqJSON([...d].values().take(2).toArray(), [1, 2], "-> take");
    eqJSON(Array.from(build.SortedSet()), [1, 2, 3, 4, 5], "Array.from works");
    /* destructuring and spread into a call */
    const [first, second] = build.List();
    assert(first === 1 && second === 2, "destructuring works");
    eqJSON(Math.max(...build.SortedSet()), 5, "spread into a call");
    /* for..of with break leaves the container untouched */
    {
        const s = build.SortedSet();
        let count = 0;
        for (const _ of s) { if (++count === 2) break; }
        eqJSON(s.toArray(), [1, 2, 3, 4, 5], "break does not disturb the container");
    }
    /* two concurrent iterations are independent */
    {
        const d2 = build.Deque();
        const a = d2[Symbol.iterator](), b = d2[Symbol.iterator]();
        a.next();
        eqJSON(b.next().value, 1, "a second iterator starts from the beginning");
    }
    /* nested loops over the same container */
    {
        const s = new SortedSet(); [1, 2].forEach(v => s.add(v));
        const pairs = [];
        for (const x of s) for (const y of s) pairs.push([x, y]);
        eqJSON(pairs, [[1, 1], [1, 2], [2, 1], [2, 2]], "nested iteration works");
    }
}

/* ---- 5. the deliberate omissions stay omitted ---- *
 *
 * Each of these lacks an element sequence for a structural reason, listed in
 * dyna-structures.c. If someone adds an iterator later it should be a decision,
 * not a drive-by, so this test has to be edited to allow it. */
{
    const noIter = [
        ["BloomFilter", new BloomFilter(64, 3)],
        ["Heap",        new Heap((a, b) => a - b)],
        ["UnionFind",   new UnionFind(4)],
        ["Fenwick",     new Fenwick(4)],
        ["SegTree",     new SegTree(4)],
        ["LRU",         new LRU(4)],
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

/* ---- 6. iteration does not leak or corrupt across many rounds ---- */
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

print("test_structures_iter: all " + n + " tests passed");
