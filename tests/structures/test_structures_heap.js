// flags: --std
// covers: dyna:structures.Heap
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_structures_heap");
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

__kitS.section("new Heap is empty", () => {
    const h = new Heap((a, b) => a - b);
        assert(h.size === 0 && h.length === 0, "new Heap is empty");
        assert(h.peek() === undefined, "peek on empty is undefined");
        assert(h.pop() === undefined, "pop on empty is undefined");
        for (const v of [5, 3, 8, 1, 9, 2, 7, 4, 6, 0])
            h.push(v);
        assert(h.size === 10, "size after 10 pushes");
        const out = [];
        while (h.size > 0) out.push(h.pop());
        assert(eqArr(out, [0, 1, 2, 3, 4, 5, 6, 7, 8, 9]),
               "pop drains in ascending order: " + JSON.stringify(out));
        assert(h.size === 0, "empty after draining");
});

__kitS.section("max-heap pop drains descending:", () => {
    const h = new Heap((a, b) => b - a);
        for (const v of [5, 3, 8, 1, 9, 2, 7, 4, 6, 0])
            h.push(v);
        const out = [];
        while (h.size > 0) out.push(h.pop());
        assert(eqArr(out, [9, 8, 7, 6, 5, 4, 3, 2, 1, 0]),
               "max-heap pop drains descending: " + JSON.stringify(out));
});

__kitS.section("peek returns the min", () => {
    const h = new Heap((a, b) => a - b);
        h.push(5); h.push(1); h.push(3);
        assert(h.peek() === 1, "peek returns the min");
        assert(h.size === 3, "peek does not change size");
        assert(h.peek() === 1, "peek is idempotent");
        assert(h.pop() === 1, "pop returns the same min peek saw");
        assert(h.size === 2, "size drops after pop");
});

__kitS.section("heapsort: size after bulk push", () => {
    const rnd = mulberry32(12345);
    const N = 5000;
    const src = [];
    for (let i = 0; i < N; i++) src.push(Math.floor(rnd() * 1e6));
    const h = new Heap((a, b) => a - b);
        for (const v of src) h.push(v);
        assert(h.size === N, "heapsort: size after bulk push");
        const out = [];
        while (h.size > 0) out.push(h.pop());
        const ref = [...src].sort((a, b) => a - b);
        assert(eqArr(out, ref), "heapsort matches Array.prototype.sort");
});

__kitS.section("random fuzz round", () => {
    const rnd = mulberry32(999);
    const h = new Heap((a, b) => a - b);
        for (let round = 0; round < 20; round++) {
            const ref = [];
            const ops = 500;
            for (let i = 0; i < ops; i++) {
                if (ref.length === 0 || rnd() < 0.6) {
                    const v = Math.floor(rnd() * 1e6);
                    h.push(v);
                    ref.push(v);
                } else {
                    ref.sort((a, b) => a - b);
                    const want = ref.shift();
                    const got = h.pop();
                    assert(got === want,
                           "random fuzz round " + round + ": got " + got +
                           " want " + want);
                }
            }
            ref.sort((a, b) => a - b);
            while (ref.length > 0) {
                const want = ref.shift();
                const got = h.pop();
                assert(got === want, "random fuzz drain round " + round);
            }
            assert(h.size === 0, "heap empty at end of fuzz round " + round);
        }
});

__kitS.section("object heap pops in key order (a)", () => {
    const h = new Heap((a, b) => a.k - b.k);
        h.push({ k: 3, name: "c" });
        h.push({ k: 1, name: "a" });
        h.push({ k: 2, name: "b" });
        assert(h.pop().name === "a", "object heap pops in key order (a)");
        assert(h.pop().name === "b", "object heap pops in key order (b)");
        assert(h.pop().name === "c", "object heap pops in key order (c)");
});

__kitS.section("Heap() with no comparator is a numeric min-heap", () => {
    const nat = new Heap();
    nat.push(3); nat.push(1); nat.push(2);
    assert(nat.pop() === 1, "Heap() with no comparator is a numeric min-heap");
    throws(() => new Heap(42), "Heap(non-function) throws");
    throws(() => new Heap("nope"), "Heap(string) throws");
    throws(() => { const h = new Heap(); h.push(1); h.push("x"); },
           "a natural heap refuses a non-number");
});

__kitS.section("NaN-comparator heap still accepts pushes", () => {
    const h = new Heap(() => NaN);
        h.push(1); h.push(2); h.push(3);
        assert(h.size === 3, "NaN-comparator heap still accepts pushes");
        const out = [h.pop(), h.pop(), h.pop()];
        assert(out.length === 3, "NaN-comparator heap drains fully");
        assert(h.size === 0, "NaN-comparator heap ends empty");
});

__kitS.section("throwing comparator surfaces during push", () => {
    const h = new Heap((a, b) => a - b);
        h.push(1); h.push(2);
        let boom = new Heap((a, b) => { throw new Error("boom"); });
            boom.push(1);
            throws(() => boom.push(2), "throwing comparator surfaces during push");
        h.push(0);
        assert(h.pop() === 0, "unrelated heap still orders correctly");
        assert(h.pop() === 1, "unrelated heap still orders correctly (2)");
        assert(h.pop() === 2, "unrelated heap still orders correctly (3)");
});

__kitS.section("size is 3 before the throwing push", () => {
    let shouldThrow = false;
    const h = new Heap((a, b) => {
        if (shouldThrow) throw new Error("boom");
        return a - b;
    });
        h.push(5); h.push(1); h.push(3);
        assert(h.size === 3, "size is 3 before the throwing push");
        shouldThrow = true;
        throws(() => h.push(2), "mid-life throwing comparator surfaces");
        shouldThrow = false;
        assert(h.size === 3,
               "size unchanged after throwing comparator (rolled back)");
        h.push(6);
        const out = [];
        while (h.size > 0) out.push(h.pop());
        const expected = [5, 1, 3, 6].sort((a, b) => a - b);
        assert(eqArr([...out].sort((a, b) => a - b), expected),
               "draining after a throwing comparator loses/duplicates nothing: " +
               JSON.stringify(out));
});

__kitS.section("reentrant push during comparator throws", () => {
    let h;
    h = new Heap((a, b) => { throws(() => h.push(999), "reentrant push during comparator throws"); return a - b; });
    h.push(3); h.push(1); h.push(2);
    assert(h.size === 3, "reentrant push attempts do not corrupt size");
    const out = [];
    while (h.size > 0) out.push(h.pop());
    assert(eqArr(out, [1, 2, 3]), "heap orders correctly despite reentrant-push attempts");

    let h2;
    h2 = new Heap((a, b) => { throws(() => h2.pop(), "reentrant pop during comparator throws"); return a - b; });
    h2.push(3); h2.push(1); h2.push(2);
    const out2 = [];
    while (h2.size > 0) out2.push(h2.pop());
    assert(eqArr(out2, [1, 2, 3]), "heap orders correctly despite reentrant-pop attempts");
});

__kitS.section("Heap has no resource surface", () => {
    const h = new Heap((a, b) => a - b);
    assert(h.close === undefined && h.closed === undefined &&
           h[Symbol.dispose] === undefined, "Heap has no resource surface");
    for (let i = 0; i < 500; i++) {
        const hh = new Heap((a, b) => a - b);
        hh.push({ i }); hh.push("s" + i); hh.push(i);
    }
    std.gc();
});
__kitS.end();

print("test_structures: all tests passed (" + n + " assertions)");
