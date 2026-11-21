// flags: --std
// covers: dyna:structures.BloomFilter
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_structures_bloom_filter");
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

__kitS.section("0 bits throws", () => {
    throws(() => new BloomFilter(0), "0 bits throws");
    const bf = new BloomFilter(4096, 5);
        assert(bf.bits === 4096 && bf.hashes === 5, "params");
        const present = [];
        const rng = mulberry32(71);
        for (let i = 0; i < 300; i++) {
            const key = "k" + ((rng() * 1e9) | 0);
            present.push(key); bf.add(key);
        }
        for (const key of present)
            assert(bf.mayContain(key) === true, "no false negative for " + key);
        let fp = 0, trials = 2000;
        for (let i = 0; i < trials; i++)
            if (bf.mayContain("absent#" + i)) fp++;
        assert(fp / trials < 0.15, "false-positive rate bounded: " + (fp / trials));
});

__kitS.section("add returns the filter", () => {
    const bf = new BloomFilter(256);
        assert(bf.add("x") === bf, "add returns the filter");
        bf.add("");
        assert(bf.mayContain("") === true, "empty key round-trips");
        assert(bf.hashes === 3, "default hashes = 3");
});
__kitS.end();

print("test_structures: all tests passed (" + n + " assertions)");
