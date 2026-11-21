// flags: --std
// covers: dyna:structures.Trie
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_structures_trie");
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

__kitS.section("empty trie", () => {
    const t = new Trie();
        assert(t.size === 0 && !t.has("x"), "empty trie");
        assert(t.insert("cat") === t, "insert returns the trie");
        t.insert("car").insert("card").insert("dog").insert("do");
        assert(t.size === 5, "size after 5 inserts");
        assert(t.has("car") && t.has("card") && t.has("do"), "has stored keys");
        assert(!t.has("ca") && !t.has("care"), "prefix/extension are not members");
        t.insert("cat");
        assert(t.size === 5, "re-insert does not grow size");
        assert(t.longestPrefix("cardance") === "card", "longestPrefix card");
        assert(t.longestPrefix("doggo") === "dog", "longestPrefix dog (> do)");
        assert(t.longestPrefix("xyz") === "", "no prefix -> empty string");
        assert(t.longestPrefix("ca") === "", "prefix present but not a key");
        assert(eqArr(t.keysWithPrefix("car").sort(), ["car", "card"]), "keysWithPrefix car");
        assert(eqArr(t.keysWithPrefix("do").sort(), ["do", "dog"]), "keysWithPrefix do");
        assert(eqArr(t.keysWithPrefix("").sort(), ["car", "card", "cat", "do", "dog"]),
               "empty prefix -> all keys");
        assert(eqArr(t.keysWithPrefix("z"), []), "no match -> []");
        assert(t.delete("car") === true, "delete existing");
        assert(!t.has("car") && t.has("card"), "delete keeps descendant keys");
        assert(t.size === 4, "size drops on delete");
        assert(t.delete("car") === false, "delete missing is false");
});

__kitS.section("empty string is a valid key", () => {
    const t = new Trie();
        t.insert("");
        assert(t.has("") && t.size === 1, "empty string is a valid key");
        assert(t.longestPrefix("anything") === "", "empty key is a prefix of all");
        assert(t.keysWithPrefix("").includes(""), "keysWithPrefix('') includes ''");
        t.insert("a");
        assert(eqArr(t.keysWithPrefix("").sort(), ["", "a"]), "empty + 'a'");
});

__kitS.section("deep key stored", () => {
    (() => {
        const t = new Trie();
        t.insert("a".repeat(10000));
        t.insert("a".repeat(10000) + "b");
        assert(t.has("a".repeat(10000)), "deep key stored");
    })();
    std.gc();
    assert(true, "deep trie freed by the GC without a C-stack overflow");
});

__kitS.section("delete return agrees @", () => {
    const rng = mulberry32(83);
    const alpha = "abc";
    const randKey = () => {
        const len = (rng() * 6) | 0;
        let s = "";
        for (let i = 0; i < len; i++) s += alpha[(rng() * alpha.length) | 0];
        return s;
    };
    for (let seed = 0; seed < 6; seed++) {
        const t = new Trie(), ref = new Set();
        for (let k = 0; k < 1500; k++) {
            const key = randKey(), op = (rng() * 3) | 0;
            if (op === 0) { t.insert(key); ref.add(key); }
            else if (op === 1) {
                const had = ref.delete(key);
                assert(t.delete(key) === had, "delete return agrees @" + k);
            } else {
                assert(t.has(key) === ref.has(key), "has agrees @" + k);
            }
        }
        assert(t.size === ref.size, "size agrees with Set");
        for (const pfx of ["", "a", "ab", "c", "zz"]) {
            const got = t.keysWithPrefix(pfx).sort();
            const want = [...ref].filter(x => x.startsWith(pfx)).sort();
            assert(eqArr(got, want), "keysWithPrefix('" + pfx + "') agrees");
        }
    }
});
__kitS.end();

print("test_structures: all tests passed (" + n + " assertions)");
