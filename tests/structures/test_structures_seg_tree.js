// flags: --std
// covers: dyna:structures.SegTree
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_structures_seg_tree");
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

__kitS.section("size 0 throws", () => {
    throws(() => new SegTree(0), "size 0 throws");
    throws(() => new SegTree(4, "bogus"), "bad op throws");
    const s = new SegTree(8);
        assert(s.size === 8, "size");
        assert(s.rangeQuery(0, 7) === 0, "sum identity is 0");
        s.update(0, 5).update(3, 2).update(7, 10);
        assert(s.rangeQuery(0, 7) === 17, "full sum");
        assert(s.rangeQuery(1, 3) === 2, "partial sum");
        assert(s.rangeQuery(3, 3) === 2, "single-index");
        assert(s.rangeQuery(4, 6) === 0, "empty region");
        assert(s.rangeQuery(5, 2) === 0, "lo>hi -> identity");
        s.update(0, 1);
        assert(s.rangeQuery(0, 0) === 1, "update reassigns");

    const mn = new SegTree(6, "min"), mx = new SegTree(6, "max");
        const vals = [5, 2, 8, 1, 9, 4];
        vals.forEach((v, i) => { mn.update(i, v); mx.update(i, v); });
        assert(mn.rangeQuery(0, 5) === 1 && mx.rangeQuery(0, 5) === 9, "global min/max");
        assert(mn.rangeQuery(1, 3) === 1 && mx.rangeQuery(1, 3) === 8, "range min/max");
        assert(mn.rangeQuery(0, 0) === 5, "single min");
        assert(mn.rangeQuery(3, 2) === Infinity, "min identity is +Inf");
        assert(mx.rangeQuery(3, 2) === -Infinity, "max identity is -Inf");
    throws(() => new SegTree(4).update(4, 1), "update out of range throws");
    throws(() => new SegTree(4).rangeQuery(0, 4), "rangeQuery out of range throws");
});

__kitS.section("rangeQuery agrees @", () => {
    const rng = mulberry32(61);
    const ops = ["sum", "min", "max"];
    for (const op of ops) {
        for (let seed = 0; seed < 4; seed++) {
            const N = 32;
            const s = new SegTree(N, op);
            const arr = new Array(N).fill(op === "sum" ? 0
                : op === "min" ? Infinity : -Infinity);
            const fold = (lo, hi) => {
                let acc = op === "sum" ? 0 : op === "min" ? Infinity : -Infinity;
                for (let j = lo; j <= hi; j++)
                    acc = op === "sum" ? acc + arr[j]
                        : op === "min" ? Math.min(acc, arr[j]) : Math.max(acc, arr[j]);
                return acc;
            };
            for (let k = 0; k < 1000; k++) {
                if ((rng() * 2) | 0) {
                    const i = (rng() * N) | 0, v = (rng() * 200 - 100) | 0;
                    s.update(i, v); arr[i] = v;
                } else {
                    let lo = (rng() * N) | 0, hi = (rng() * N) | 0;
                    if (lo > hi) { const t = lo; lo = hi; hi = t; }
                    assert(s.rangeQuery(lo, hi) === fold(lo, hi),
                           op + " rangeQuery agrees @" + k);
                }
            }
        }
    }
});
__kitS.end();

print("test_structures: all tests passed (" + n + " assertions)");
