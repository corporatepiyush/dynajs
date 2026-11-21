// flags: --std
// covers: dyna:structures.BitSet
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_structures_bit_set");
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

__kitS.section("empty BitSet count 0", () => {
    const b = new BitSet();
        assert(b.count === 0, "empty BitSet count 0");
        assert(b.get(0) === false, "get on empty is false");
        assert(b.get(1 << 20) === false, "get far past end is false (no grow)");
        assert(b.set(3) === b, "set returns the BitSet (chainable)");
        b.set(65).set(130);
        assert(b.count === 3, "count after 3 sets");
        assert(b.get(3) && b.get(65) && b.get(130), "bits are set");
        assert(!b.get(64) && !b.get(2) && !b.get(129), "neighbours untouched");
        b.clear(65);
        assert(!b.get(65) && b.count === 2, "clear drops a bit");
        b.clear(999);
        assert(b.count === 2, "clear of unset bit is a no-op");
        b.flip(3);
        assert(!b.get(3) && b.count === 1, "flip clears a set bit");
        b.flip(7);
        assert(b.get(7) && b.count === 2, "flip sets an unset bit");
});

__kitS.section("word-boundary bits count", () => {
    const b = new BitSet();
        for (const i of [0, 63, 64, 65, 127, 128])
            b.set(i);
        assert(b.count === 6, "word-boundary bits count");
        for (const i of [0, 63, 64, 65, 127, 128])
            assert(b.get(i), "word-boundary bit " + i + " set");
        assert(!b.get(62) && !b.get(126), "between-boundary bits clear");
});

__kitS.section("nextSet from 0", () => {
    const b = new BitSet();
        b.set(5).set(64).set(200);
        assert(b.nextSet(0) === 5, "nextSet from 0");
        assert(b.nextSet(5) === 5, "nextSet includes the from index");
        assert(b.nextSet(6) === 64, "nextSet skips to next word");
        assert(b.nextSet(65) === 200, "nextSet across empty words");
        assert(b.nextSet(201) === -1, "nextSet past last is -1");
        assert(b.nextSet(1 << 20) === -1, "nextSet far past end is -1");
    const empty = new BitSet();
    assert(empty.nextSet(0) === -1, "nextSet on empty is -1");
});

__kitS.section("and: intersection", () => {
    const a = new BitSet(), b = new BitSet();
        a.set(1).set(3).set(64).set(200);
        b.set(3).set(64).set(65);
        const or = new BitSet(); or.set(1).set(3).set(64).set(65).set(200);
        const av = a.toArray();
        const aClone = new BitSet(); for (const i of av) aClone.set(i);
        aClone.and(b);
        assert(eqArr(aClone.toArray(), [3, 64]), "and: intersection");
        const aClone2 = new BitSet(); for (const i of av) aClone2.set(i);
        aClone2.or(b);
        assert(eqArr(aClone2.toArray(), or.toArray()), "or: union");
        const aClone3 = new BitSet(); for (const i of av) aClone3.set(i);
        aClone3.xor(b);
        assert(eqArr(aClone3.toArray(), [1, 65, 200]), "xor: symmetric difference");
        const aClone4 = new BitSet(); for (const i of av) aClone4.set(i);
        aClone4.and(aClone4);
        assert(eqArr(aClone4.toArray(), av), "and with self is identity");
});

__kitS.section("get agrees @", () => {
    const rng = mulberry32(9);
    for (let seed = 0; seed < 8; seed++) {
        const b = new BitSet(), ref = new Set();
        for (let k = 0; k < 4000; k++) {
            const i = (rng() * 500) | 0, op = (rng() * 3) | 0;
            if (op === 0) { b.set(i); ref.add(i); }
            else if (op === 1) { b.clear(i); ref.delete(i); }
            else { assert(b.get(i) === ref.has(i), "get agrees @" + k); }
        }
        assert(b.count === ref.size, "count agrees with Set.size");
        const got = b.toArray();
        const want = [...ref].sort((x, y) => x - y);
        assert(eqArr(got, want), "toArray agrees with sorted Set");
    }
});

__kitS.section("has no .close", () => {
    const probes = [new BitSet(), new UnionFind(4), new Deque(), new Fenwick(4),
                    new RingBuffer(4), new SegTree(4), new BloomFilter(256),
                    new Trie(), new LRU(2), new SortedSet(), new SortedMap()];
    for (const o of probes) {
        const nm = o.constructor.name;
        assert(o.close === undefined, nm + " has no .close");
        assert(o.closed === undefined, nm + " has no .closed");
        assert(o[Symbol.dispose] === undefined, nm + " has no [Symbol.dispose]");
        assert(typeof o === "object", nm + " is a plain object");
    }
});

__kitS.section("extra", () => {
    for (let i = 0; i < 2000; i++) {
        const b = new BitSet(); b.set(i & 255).set((i * 7) & 511);
        const uf = new UnionFind(8); uf.union(0, i & 7);
        const d = new Deque(); d.pushBack({ i }); d.pushFront("s" + i);
        const f = new Fenwick(8); f.update(i & 7, i);
        const rb = new RingBuffer(4); rb.push({ i }); rb.push("s" + i); rb.push(i);
        const sg = new SegTree(8, i & 1 ? "min" : "sum"); sg.update(i & 7, i);
        const bf = new BloomFilter(512, 4); bf.add("x" + i); bf.add("y" + i);
        const tr = new Trie(); tr.insert("k" + i).insert("k" + i + "extra");
        const lru = new LRU(3); lru.put("a", { i }); lru.put("b", "s" + i); lru.put("c", i); lru.put("d", i);
        const ss = new SortedSet(); for (let k = 0; k < 8; k++) ss.add((i * 13 + k) & 63);
        const sm = new SortedMap(); for (let k = 0; k < 8; k++) sm.set(k, { i, k });
    }
    std.gc();
});
__kitS.end();

print("test_structures: all tests passed (" + n + " assertions)");
