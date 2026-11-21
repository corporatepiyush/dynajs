// flags: --std
// covers: dyna:structures.LRU
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_structures_lru");
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

__kitS.section("capacity 0 throws", () => {
    throws(() => new LRU(0), "capacity 0 throws");
    throws(() => new LRU(), "missing capacity throws");
    const c = new LRU(2);
        assert(c.capacity === 2 && c.size === 0, "empty cache");
        assert(c.get("x") === undefined, "get missing is undefined");
        assert(c.put(1, "a") === c, "put returns the cache (chainable)");
        c.put(2, "b");
        assert(c.size === 2 && c.get(1) === "a" && c.get(2) === "b", "stored");
        c.get(1);
        c.put(3, "c");
        assert(!c.has(2), "LRU key evicted");
        assert(c.get(1) === "a" && c.get(3) === "c", "MRU keys kept");
        assert(c.size === 2, "size stays at capacity");
        c.put(1, "A");
        assert(c.get(1) === "A" && c.size === 2, "update in place");
        assert(c.delete(3) === true && !c.has(3) && c.size === 1, "delete");
        assert(c.delete(9) === false, "delete missing is false");
        assert(c.has(1) === true, "has does not mutate recency");
});

__kitS.section("capacity 1 evicts", () => {
    const c = new LRU(1);
        c.put("a", 1); c.put("b", 2);
        assert(!c.has("a") && c.get("b") === 2 && c.size === 1, "capacity 1 evicts");
});

__kitS.section("object identity", () => {
    const c = new LRU(4);
        const o = { k: 1 };
        c.put("o", o); c.put("s", "str"); c.put("n", 3.5);
        assert(c.get("o") === o && c.get("o").k === 1, "object identity");
        assert(c.get("s") === "str" && c.get("n") === 3.5, "mixed values");
});

__kitS.section("get value agrees @", () => {
    const rng = mulberry32(97);
    for (let seed = 0; seed < 6; seed++) {
        const cap = 1 + ((rng() * 6) | 0);
        const c = new LRU(cap);
        const ref = new Map();
        const touch = (k, v) => { ref.delete(k); ref.set(k, v); };
        const evictIfNeeded = () => {
            while (ref.size > cap) { const oldest = ref.keys().next().value; ref.delete(oldest); }
        };
        for (let step = 0; step < 3000; step++) {
            const k = "k" + ((rng() * 12) | 0), op = (rng() * 3) | 0;
            if (op === 0) {
                const v = (rng() * 1000) | 0;
                c.put(k, v); touch(k, v); evictIfNeeded();
            } else if (op === 1) {
                const got = c.get(k);
                if (ref.has(k)) { const v = ref.get(k); touch(k, v);
                    assert(got === v, "get value agrees @" + step); }
                else assert(got === undefined, "get miss agrees @" + step);
            } else {
                assert(c.has(k) === ref.has(k), "has agrees @" + step);
            }
            assert(c.size === ref.size, "size agrees @" + step);
        }
        for (const [k, v] of ref)
            assert(c.get(k) === v, "surviving key value agrees");
    }
});

__kitS.section("fill far past capacity keeps exactly the capacity", () => {
    const cap = 100, N = 5000;
    const c = new LRU(cap);
    for (let i = 0; i < N; i++) c.put("key:" + i, i * 2);
    assert(c.size === cap, "fill far past capacity keeps exactly the capacity");
    for (let i = 0; i < N - cap; i++)
        assert(!c.has("key:" + i), "early key " + i + " was evicted");
    for (let i = N - cap; i < N; i++)
        assert(c.get("key:" + i) === i * 2, "survivor " + i + " value intact");
    const ents = c.entries();
    assert(ents.length === cap, "entries() sees every survivor");
    assert(ents[0][0] === "key:" + (N - 1) && ents[0][1] === (N - 1) * 2,
           "entries() starts at the MRU");
    assert(ents[cap - 1][0] === "key:" + (N - cap),
           "entries() ends at the least recent survivor");
    for (let i = 0; i < 10; i++) c.put("fresh:" + i, i);
    assert(!c.has("key:" + (N - cap)), "the next put evicts the LRU end");
    assert(c.has("key:" + (N - cap + 10)), "and only the LRU end");
    for (let i = 0; i < 10; i++)
        assert(c.get("fresh:" + i) === i, "fresh keys are all found");
});
__kitS.end();

print("test_structures: all tests passed (" + n + " assertions)");
