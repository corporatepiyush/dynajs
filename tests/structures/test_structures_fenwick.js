// flags: --std
// covers: dyna:structures.Fenwick
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_structures_fenwick");
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

__kitS.section("all zero initially", () => {
    const f = new Fenwick(8);
        assert(f.size === 8, "size");
        assert(f.prefixSum(7) === 0, "all zero initially");
        assert(f.update(0, 5) === f, "update returns the Fenwick (chainable)");
        f.update(3, 2).update(7, 10);
        assert(f.prefixSum(0) === 5, "prefixSum(0)");
        assert(f.prefixSum(3) === 7, "prefixSum(3) = 5+2");
        assert(f.prefixSum(7) === 17, "prefixSum(7) = 5+2+10");
        assert(f.rangeQuery(1, 7) === 12, "rangeQuery(1,7) = 17-5");
        assert(f.rangeQuery(4, 6) === 0, "rangeQuery over empty span");
        assert(f.rangeQuery(3, 3) === 2, "rangeQuery single index");
        assert(f.rangeQuery(5, 2) === 0, "rangeQuery lo>hi is empty (0)");
        f.update(3, -2);
        assert(f.prefixSum(3) === 5, "negative delta subtracts");
});

__kitS.section("update out of range throws", () => {
    const f = new Fenwick(4);
        throws(() => f.update(4, 1), "update out of range throws");
        throws(() => f.update(-1, 1), "update negative index throws");
        throws(() => f.prefixSum(4), "prefixSum out of range throws");
        throws(() => f.rangeQuery(0, 4), "rangeQuery hi out of range throws");
        throws(() => f.rangeQuery(-1, 2), "rangeQuery negative lo throws");
});

__kitS.section("prefixSum agrees @", () => {
    const rng = mulberry32(41);
    for (let seed = 0; seed < 8; seed++) {
        const N = 64;
        const f = new Fenwick(N), arr = new Array(N).fill(0);
        for (let k = 0; k < 1500; k++) {
            const op = (rng() * 3) | 0;
            if (op === 0) {
                const i = (rng() * N) | 0, delta = (rng() * 200 - 100) | 0;
                f.update(i, delta); arr[i] += delta;
            } else if (op === 1) {
                const i = (rng() * N) | 0;
                let s = 0; for (let j = 0; j <= i; j++) s += arr[j];
                assert(f.prefixSum(i) === s, "prefixSum agrees @" + k);
            } else {
                let lo = (rng() * N) | 0, hi = (rng() * N) | 0;
                let s = 0; for (let j = lo; j <= hi; j++) s += arr[j];
                assert(f.rangeQuery(lo, hi) === s, "rangeQuery agrees @" + k);
            }
        }
    }
});
__kitS.end();

print("test_structures: all tests passed (" + n + " assertions)");
