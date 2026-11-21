// flags: --std
// covers: dyna:structures.RingBuffer
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_structures_ring_buffer");
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

__kitS.section("capacity 0 throws RangeError", () => {
    throws(() => new RingBuffer(0), "capacity 0 throws RangeError");
    throws(() => new RingBuffer(), "missing capacity throws");
    const r = new RingBuffer(3);
        assert(r.capacity === 3, "capacity");
        assert(r.length === 0 && !r.full, "empty");
        r.push(1); r.push(2);
        assert(eqArr(r.toArray(), [1, 2]) && !r.full, "partial fill");
        r.push(3);
        assert(r.full && eqArr(r.toArray(), [1, 2, 3]), "full at capacity");
        r.push(4);
        assert(eqArr(r.toArray(), [2, 3, 4]), "overwrite oldest");
        r.push(5); r.push(6);
        assert(eqArr(r.toArray(), [4, 5, 6]), "keeps most recent capacity items");
        assert(r.get(0) === 4 && r.get(2) === 6, "get oldest..newest");
        assert(r.get(3) === undefined, "get past count undefined");
        assert(r.length === 3, "length capped at capacity");
});

__kitS.section("post-evict contents", () => {
    const r = new RingBuffer(2);
        const o = { k: 1 };
        r.push(o); r.push("s"); r.push(3.5);
        assert(r.get(0) === "s" && r.get(1) === 3.5, "post-evict contents");
});

__kitS.section("ring window agrees @seed", () => {
    const rng = mulberry32(53);
    for (let seed = 0; seed < 6; seed++) {
        const cap = 1 + ((rng() * 8) | 0);
        const r = new RingBuffer(cap), ref = [];
            for (let k = 0; k < 3000; k++) {
                const x = (rng() * 1000) | 0;
                r.push(x); ref.push(x);
                if (ref.length > cap) ref.shift();
            }
            assert(eqArr(r.toArray(), ref), "ring window agrees @seed " + seed);
            assert(r.length === ref.length, "ring length agrees");
    }
});
__kitS.end();

print("test_structures: all tests passed (" + n + " assertions)");
