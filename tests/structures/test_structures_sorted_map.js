// flags: --std
// covers: dyna:structures.SortedMap
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_structures_sorted_map");
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

__kitS.section("set returns the map (chainable)", () => {
    const m = new SortedMap();
        assert(m.size === 0 && m.firstKey() === undefined, "empty");
        assert(m.set(30, "c") === m, "set returns the map (chainable)");
        m.set(10, "a").set(20, "b");
        assert(m.size === 3 && m.get(20) === "b", "get");
        assert(m.get(99) === undefined, "get missing is undefined");
        assert(eqArr(m.keys(), [10, 20, 30]), "keys ascending");
        assert(m.firstKey() === 10 && m.lastKey() === 30, "first/last key");
        assert(m.floorKey(25) === 20 && m.ceilKey(25) === 30, "floor/ceil key");
        assert(m.floorKey(5) === undefined && m.ceilKey(40) === undefined, "out-of-range floor/ceil");
        m.set(20, "B");
        assert(m.get(20) === "B" && m.size === 3, "update in place");
        assert(m.has(10) && !m.has(15), "has");
        const r = m.rangeQuery(10, 20);
        assert(r.length === 2 && r[0][0] === 10 && r[0][1] === "a" &&
               r[1][0] === 20 && r[1][1] === "B", "rangeQuery [k,v] pairs");
        assert(m.delete(10) === true && !m.has(10) && m.size === 2, "delete");
        assert(m.delete(10) === false, "delete missing is false");
    throws(() => new SortedMap().set(NaN, 1), "set NaN key throws");
});

__kitS.section("object identity", () => {
    const m = new SortedMap();
        const o = { k: 1 };
        m.set(1, o); m.set(2, "s"); m.set(3, 3.5);
        assert(m.get(1) === o && m.get(1).k === 1, "object identity");
        assert(m.get(2) === "s" && m.get(3) === 3.5, "mixed values");
});

__kitS.section("delete agrees @", () => {
    const rng = mulberry32(109);
    for (let seed = 0; seed < 8; seed++) {
        const m = new SortedMap(), ref = new Map();
        const sortedKeys = () => [...ref.keys()].sort((a, b) => a - b);
        for (let k = 0; k < 4000; k++) {
            const key = (rng() * 200) | 0, op = (rng() * 4) | 0;
            if (op === 0) { const v = (rng() * 1000) | 0; m.set(key, v); ref.set(key, v); }
            else if (op === 1) { assert(m.delete(key) === ref.delete(key), "delete agrees @" + k); }
            else if (op === 2) {
                assert(m.get(key) === (ref.has(key) ? ref.get(key) : undefined), "get agrees @" + k);
            } else {
                const keys = sortedKeys();
                let fk, ck;
                for (const kk of keys) { if (kk <= key) fk = kk; }
                for (const kk of keys) { if (kk >= key) { ck = kk; break; } }
                assert(m.floorKey(key) === (fk === undefined ? undefined : fk), "floorKey agrees @" + k);
                assert(m.ceilKey(key) === (ck === undefined ? undefined : ck), "ceilKey agrees @" + k);
            }
        }
        assert(m.size === ref.size, "size agrees");
        assert(eqArr(m.keys(), sortedKeys()), "key order agrees");
        for (const key of sortedKeys())
            assert(m.get(key) === ref.get(key), "value agrees for key " + key);
    }
});
__kitS.end();

print("test_structures: all tests passed (" + n + " assertions)");
