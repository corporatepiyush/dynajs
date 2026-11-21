// flags: --std
// covers: dyna:structures.Deque
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_structures_deque");
import { BitSet, UnionFind, Deque, Fenwick, RingBuffer, SegTree, BloomFilter,
         Trie, LRU, SortedSet, SortedMap, Heap, List } from "dyna:structures";
import * as std from "std";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}
function throws(fn, msg) {
    let threw = false;
    try { fn(); } catch { threw = true; }
    assert(threw, msg);
}
function eqArr(a, b) {
    if (a.length !== b.length) return false;
    for (let i = 0; i < a.length; i++)
        if (!Object.is(a[i], b[i])) return false;
    return true;
}
function mulberry32(seed) {
    let a = seed >>> 0;
    return function () {
        a |= 0; a = (a + 0x6D2B79F5) | 0;
        let t = Math.imul(a ^ (a >>> 15), 1 | a);
        t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
        return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    };
}

__kitS.section("empty length", () => {
    const d = new Deque();
        assert(d.length === 0, "empty length");
        assert(d.popFront() === undefined && d.popBack() === undefined, "pop empty");
        assert(d.peekFront() === undefined && d.peekBack() === undefined, "peek empty");
        assert(d.pushBack(1) === 1, "pushBack returns length");
        assert(d.pushBack(2) === 2, "pushBack grows");
        assert(d.pushFront(0) === 3, "pushFront returns length");
        assert(eqArr(d.toArray(), [0, 1, 2]), "order front..back");
        assert(d.get(0) === 0 && d.get(2) === 2, "get by front index");
        assert(d.get(3) === undefined, "get past end undefined");
        assert(d.peekFront() === 0 && d.peekBack() === 2, "peeks");
        assert(d.popFront() === 0, "popFront");
        assert(d.popBack() === 2, "popBack");
        assert(d.length === 1 && d.get(0) === 1, "middle remains");
});

__kitS.section("object identity", () => {
    const d = new Deque();
        const o = { tag: 1 };
        d.pushBack(o); d.pushBack("s"); d.pushFront(3.5);
        assert(d.get(1) === o && d.get(1).tag === 1, "object identity");
        assert(d.popBack() === "s" && d.popFront() === 3.5, "mixed values");
});

__kitS.section("pushBack len @", () => {
    const rng = mulberry32(31);
    for (let seed = 0; seed < 8; seed++) {
        const d = new Deque(), ref = [];
        for (let k = 0; k < 4000; k++) {
            const op = (rng() * 6) | 0, x = (rng() * 1000) | 0;
            if (op === 0) { assert(d.pushBack(x) === ref.push(x), "pushBack len @" + k); }
            else if (op === 1) { ref.unshift(x); assert(d.pushFront(x) === ref.length, "pushFront len @" + k); }
            else if (op === 2) { assert(d.popFront() === (ref.length ? ref.shift() : undefined), "popFront @" + k); }
            else if (op === 3) { assert(d.popBack() === (ref.length ? ref.pop() : undefined), "popBack @" + k); }
            else if (op === 4) { assert(d.length === ref.length, "length @" + k); }
            else if (op === 5 && ref.length) { const i = (rng() * ref.length) | 0;
                assert(d.get(i) === ref[i], "get @" + k); }
        }
        assert(eqArr(d.toArray(), ref), "final contents agree");
    }
});

__kitS.section("reference cycle through a value-holding structure was reclaimed (gc_mark works)", () => {
    function cycleRef(make, store) {
        const c = make();
        const o = { back: c };
        store(c, o);
        return new WeakRef(c);
    }
    const refs = [
        cycleRef(() => new Deque(), (c, o) => c.pushBack(o)),
        cycleRef(() => new RingBuffer(4), (c, o) => c.push(o)),
        cycleRef(() => new LRU(4), (c, o) => c.put("k", o)),
        cycleRef(() => new SortedMap(), (c, o) => c.set(1, o)),
    ];
    std.gc();
    for (const w of refs)
        assert(w.deref() === undefined,
               "reference cycle through a value-holding structure was reclaimed (gc_mark works)");
});
__kitS.end();

print("test_structures: all tests passed (" + n + " assertions)");
