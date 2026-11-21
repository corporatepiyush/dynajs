// flags: --std
// covers: dyna:structures.UnionFind
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_structures_union_find");
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

__kitS.section("each element its own set initially", () => {
    const uf = new UnionFind(6);
        assert(uf.size === 6, "size");
        assert(uf.count === 6, "each element its own set initially");
        for (let i = 0; i < 6; i++) assert(uf.find(i) === i, "self-parent @" + i);
        assert(uf.union(0, 1) === true, "union merges distinct sets");
        assert(uf.count === 5, "count drops on real merge");
        assert(uf.union(0, 1) === false, "union of already-connected is false");
        assert(uf.count === 5, "count unchanged on redundant union");
        uf.union(2, 3); uf.union(1, 3);
        assert(uf.connected(0, 3) === true, "transitively connected");
        assert(uf.connected(0, 4) === false, "unconnected");
        assert(uf.find(0) === uf.find(2), "same representative");
        assert(uf.count === 3, "3 components: {0,1,2,3},{4},{5}");
});

__kitS.section("empty UnionFind", () => {
    const uf0 = new UnionFind(0);
    assert(uf0.size === 0 && uf0.count === 0, "empty UnionFind");
    const uf1 = new UnionFind(1);
        assert(uf1.find(0) === 0, "singleton find");
        assert(uf1.union(0, 0) === false, "union with self is false");
        assert(uf1.count === 1, "singleton count");
    const uf = new UnionFind(4);
        throws(() => uf.find(4), "find out of range throws RangeError");
        throws(() => uf.union(0, 9), "union out of range throws");
        throws(() => uf.connected(9, 0), "connected out of range throws");
});

__kitS.section("union return agrees @", () => {
    const rng = mulberry32(17);
    for (let seed = 0; seed < 8; seed++) {
        const N = 60;
        const uf = new UnionFind(N);
        const label = new Array(N);
        for (let i = 0; i < N; i++) label[i] = i;
        for (let k = 0; k < 2000; k++) {
            const x = (rng() * N) | 0, y = (rng() * N) | 0;
            if ((rng() * 2) | 0) {
                const merged = uf.union(x, y);
                const refMerged = label[x] !== label[y];
                assert(merged === refMerged, "union return agrees @" + k);
                if (refMerged) {
                    const from = label[y], to = label[x];
                    for (let i = 0; i < N; i++) if (label[i] === from) label[i] = to;
                }
            } else {
                assert(uf.connected(x, y) === (label[x] === label[y]),
                       "connected agrees @" + k);
            }
        }
        const distinct = new Set(label).size;
        assert(uf.count === distinct, "component count agrees");
    }
});
__kitS.end();

print("test_structures: all tests passed (" + n + " assertions)");
