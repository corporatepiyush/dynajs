// flags: --std
// covers: dyna:structures.SortedSet
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_structures_sorted_set");
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

__kitS.section("empty first/last", () => {
    const s = new SortedSet();
        assert(s.size === 0, "empty");
        assert(s.first() === undefined && s.last() === undefined, "empty first/last");
        assert(s.floor(5) === undefined && s.ceil(5) === undefined, "empty floor/ceil");
        assert(s.add(5) === s, "add returns the set (chainable)");
        for (const x of [1, 9, 3, 7, 1, 3]) s.add(x);
        assert(s.size === 5, "duplicates ignored");
        assert(eqArr(s.toArray(), [1, 3, 5, 7, 9]), "ascending order");
        assert(s.first() === 1 && s.last() === 9, "first/last");
        assert(s.has(7) && !s.has(8), "has");
        assert(s.floor(4) === 3 && s.ceil(4) === 5, "floor/ceil between keys");
        assert(s.floor(5) === 5 && s.ceil(5) === 5, "floor/ceil on a key");
        assert(s.floor(0) === undefined, "floor below all is undefined");
        assert(s.ceil(10) === undefined, "ceil above all is undefined");
        assert(eqArr(s.rangeQuery(3, 7), [3, 5, 7]), "rangeQuery inclusive");
        assert(eqArr(s.rangeQuery(4, 6), [5]), "rangeQuery partial");
        assert(eqArr(s.rangeQuery(7, 3), []), "rangeQuery lo>hi is empty");
        assert(eqArr(s.rangeQuery(-5, 100), [1, 3, 5, 7, 9]), "rangeQuery covering all");
        assert(s.delete(3) === true && !s.has(3) && s.size === 4, "delete");
        assert(s.delete(3) === false, "delete missing is false");
    throws(() => new SortedSet().add(NaN), "add NaN throws");
});

__kitS.section("negatives + floats ordered", () => {
    const s = new SortedSet();
        for (const x of [-2.5, 3.14, -10, 0, 3.14]) s.add(x);
        assert(eqArr(s.toArray(), [-10, -2.5, 0, 3.14]), "negatives + floats ordered");
        assert(s.floor(-3) === -10 && s.ceil(-3) === -2.5, "negative floor/ceil");
});

__kitS.section("delete agrees @", () => {
    const rng = mulberry32(103);
    for (let seed = 0; seed < 8; seed++) {
        const s = new SortedSet(), ref = new Set();
        const sorted = () => [...ref].sort((a, b) => a - b);
        for (let k = 0; k < 4000; k++) {
            const x = (rng() * 200) | 0, op = (rng() * 4) | 0;
            if (op === 0) { s.add(x); ref.add(x); }
            else if (op === 1) { assert(s.delete(x) === ref.delete(x), "delete agrees @" + k); }
            else if (op === 2) { assert(s.has(x) === ref.has(x), "has agrees @" + k); }
            else {
                const arr = sorted();
                let floor, ceil;
                for (const v of arr) { if (v <= x) floor = v; }
                for (const v of arr) { if (v >= x) { ceil = v; break; } }
                assert(s.floor(x) === (floor === undefined ? undefined : floor), "floor agrees @" + k);
                assert(s.ceil(x) === (ceil === undefined ? undefined : ceil), "ceil agrees @" + k);
            }
        }
        assert(s.size === ref.size, "size agrees");
        assert(eqArr(s.toArray(), sorted()), "final order agrees");
        const lo = 40, hi = 160;
        assert(eqArr(s.rangeQuery(lo, hi), sorted().filter(v => v >= lo && v <= hi)),
               "rangeQuery agrees");
    }
});
__kitS.end();

print("test_structures: all tests passed (" + n + " assertions)");
