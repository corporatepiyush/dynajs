/* test_structures.js — dyna:structures: data structures JS has NO builtin for
 * (BitSet, UnionFind, Deque, Fenwick, SegTree, RingBuffer, BloomFilter, Trie,
 * LRU, SortedSet, SortedMap, plus Heap/List merged in from the former
 * dyna:container). Array/Map/Set/TypedArray are engine intrinsics and are
 * deliberately NOT reimplemented here.
 *
 * These are PLAIN GC-managed objects (like Map/Set): no .close()/.closed/
 * [Symbol.dispose]. Covers, per class: the value/behaviour matrix, a randomized
 * differential oracle vs a trivially-correct JS reference, bounds/error paths,
 * plus GC lifecycle — finalizer reclamation AND reference-cycle collection
 * (which proves the value-holders' gc_mark).
 *
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_structures.js
 * Prints "test_structures: all tests passed" on success; throws on failure. */

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

/* =====================================================================
 * BitSet
 * ===================================================================== */

/* ---------------- set / get / clear / flip / count ---------------- */
{
    const b = new BitSet();
        assert(b.count === 0, "empty BitSet count 0");
        assert(b.get(0) === false, "get on empty is false");
        assert(b.get(1 << 20) === false, "get far past end is false (no grow)");
        assert(b.set(3) === b, "set returns the BitSet (chainable)");
        b.set(65).set(130);                 /* span 3 words */
        assert(b.count === 3, "count after 3 sets");
        assert(b.get(3) && b.get(65) && b.get(130), "bits are set");
        assert(!b.get(64) && !b.get(2) && !b.get(129), "neighbours untouched");
        b.clear(65);
        assert(!b.get(65) && b.count === 2, "clear drops a bit");
        b.clear(999);                        /* clearing an unset far bit is ok */
        assert(b.count === 2, "clear of unset bit is a no-op");
        b.flip(3);                           /* set -> unset */
        assert(!b.get(3) && b.count === 1, "flip clears a set bit");
        b.flip(7);                           /* unset -> set */
        assert(b.get(7) && b.count === 2, "flip sets an unset bit");
}

/* ---------------- word-boundary bits (63/64/65, 0) ---------------- */
{
    const b = new BitSet();
        for (const i of [0, 63, 64, 65, 127, 128])
            b.set(i);
        assert(b.count === 6, "word-boundary bits count");
        for (const i of [0, 63, 64, 65, 127, 128])
            assert(b.get(i), "word-boundary bit " + i + " set");
        assert(!b.get(62) && !b.get(126), "between-boundary bits clear");
}

/* ---------------- nextSet ---------------- */
{
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
}

/* ---------------- and / or / xor (with another BitSet) ---------------- */
{
    const a = new BitSet(), b = new BitSet();
        a.set(1).set(3).set(64).set(200);
        b.set(3).set(64).set(65);
        const or = new BitSet(); or.set(1).set(3).set(64).set(65).set(200);
        const av = a.toArray();
        /* a AND b */
        const aClone = new BitSet(); for (const i of av) aClone.set(i);
        aClone.and(b);
        assert(eqArr(aClone.toArray(), [3, 64]), "and: intersection");
        /* a OR b */
        const aClone2 = new BitSet(); for (const i of av) aClone2.set(i);
        aClone2.or(b);
        assert(eqArr(aClone2.toArray(), or.toArray()), "or: union");
        /* a XOR b */
        const aClone3 = new BitSet(); for (const i of av) aClone3.set(i);
        aClone3.xor(b);
        assert(eqArr(aClone3.toArray(), [1, 65, 200]), "xor: symmetric difference");
        /* self-op: a AND a == a */
        const aClone4 = new BitSet(); for (const i of av) aClone4.set(i);
        aClone4.and(aClone4);
        assert(eqArr(aClone4.toArray(), av), "and with self is identity");
}

/* ---------------- BitSet differential vs a JS Set<number> ---------------- */
{
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
}

/* =====================================================================
 * UnionFind
 * ===================================================================== */

/* ---------------- find / union / connected / count / size ---------------- */
{
    const uf = new UnionFind(6);
        assert(uf.size === 6, "size");
        assert(uf.count === 6, "each element its own set initially");
        for (let i = 0; i < 6; i++) assert(uf.find(i) === i, "self-parent @" + i);
        assert(uf.union(0, 1) === true, "union merges distinct sets");
        assert(uf.count === 5, "count drops on real merge");
        assert(uf.union(0, 1) === false, "union of already-connected is false");
        assert(uf.count === 5, "count unchanged on redundant union");
        uf.union(2, 3); uf.union(1, 3);         /* {0,1,2,3} one component */
        assert(uf.connected(0, 3) === true, "transitively connected");
        assert(uf.connected(0, 4) === false, "unconnected");
        assert(uf.find(0) === uf.find(2), "same representative");
        assert(uf.count === 3, "3 components: {0,1,2,3},{4},{5}");
}

/* ---------------- degenerate sizes + bounds ---------------- */
{
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
}

/* ---------------- UnionFind differential vs a JS label oracle ---------------- */
{
    const rng = mulberry32(17);
    for (let seed = 0; seed < 8; seed++) {
        const N = 60;
        const uf = new UnionFind(N);
        const label = new Array(N);            /* oracle: component id per element */
        for (let i = 0; i < N; i++) label[i] = i;
        for (let k = 0; k < 2000; k++) {
            const x = (rng() * N) | 0, y = (rng() * N) | 0;
            if ((rng() * 2) | 0) {
                const merged = uf.union(x, y);
                const refMerged = label[x] !== label[y];
                assert(merged === refMerged, "union return agrees @" + k);
                if (refMerged) {                /* relabel y's component to x's */
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
}

/* =====================================================================
 * Deque
 * ===================================================================== */

/* ---------------- push/pop/peek both ends, get, length, toArray ---------- */
{
    const d = new Deque();
        assert(d.length === 0, "empty length");
        assert(d.popFront() === undefined && d.popBack() === undefined, "pop empty");
        assert(d.peekFront() === undefined && d.peekBack() === undefined, "peek empty");
        assert(d.pushBack(1) === 1, "pushBack returns length");
        assert(d.pushBack(2) === 2, "pushBack grows");
        assert(d.pushFront(0) === 3, "pushFront returns length");
        assert(eqArr(d.toArray(), [0, 1, 2]), "order front..back");
        assert(d.get(0) === 0 && d.get(2) === 2, "get by front index");
        assert(d.get(3) === undefined, "get past end undefined");
        assert(d.peekFront() === 0 && d.peekBack() === 2, "peeks");
        assert(d.popFront() === 0, "popFront");
        assert(d.popBack() === 2, "popBack");
        assert(d.length === 1 && d.get(0) === 1, "middle remains");
}

/* ---------------- stores any JS value; identity preserved ---------------- */
{
    const d = new Deque();
        const o = { tag: 1 };
        d.pushBack(o); d.pushBack("s"); d.pushFront(3.5);
        assert(d.get(1) === o && d.get(1).tag === 1, "object identity");
        assert(d.popBack() === "s" && d.popFront() === 3.5, "mixed values");
}

/* ---------------- ring wrap-around under interleaved ops (differential) --- */
{
    const rng = mulberry32(31);
    for (let seed = 0; seed < 8; seed++) {
        const d = new Deque(), ref = [];       /* oracle: a plain array */
        for (let k = 0; k < 4000; k++) {
            const op = (rng() * 6) | 0, x = (rng() * 1000) | 0;
            if (op === 0) { assert(d.pushBack(x) === ref.push(x), "pushBack len @" + k); }
            else if (op === 1) { ref.unshift(x); assert(d.pushFront(x) === ref.length, "pushFront len @" + k); }
            else if (op === 2) { assert(d.popFront() === (ref.length ? ref.shift() : undefined), "popFront @" + k); }
            else if (op === 3) { assert(d.popBack() === (ref.length ? ref.pop() : undefined), "popBack @" + k); }
            else if (op === 4) { assert(d.length === ref.length, "length @" + k); }
            else if (op === 5 && ref.length) { const i = (rng() * ref.length) | 0;
                assert(d.get(i) === ref[i], "get @" + k); }
        }
        assert(eqArr(d.toArray(), ref), "final contents agree");
    }
}

/* =====================================================================
 * Fenwick
 * ===================================================================== */

/* ---------------- update / prefixSum / rangeQuery ---------------- */
{
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
        f.update(3, -2);                          /* negative delta */
        assert(f.prefixSum(3) === 5, "negative delta subtracts");
}

/* ---------------- bounds ---------------- */
{
    const f = new Fenwick(4);
        throws(() => f.update(4, 1), "update out of range throws");
        throws(() => f.update(-1, 1), "update negative index throws");
        throws(() => f.prefixSum(4), "prefixSum out of range throws");
        throws(() => f.rangeQuery(0, 4), "rangeQuery hi out of range throws");
        throws(() => f.rangeQuery(-1, 2), "rangeQuery negative lo throws");
}

/* ---------------- Fenwick differential vs a recomputed prefix-sum array --- */
{
    const rng = mulberry32(41);
    for (let seed = 0; seed < 8; seed++) {
        const N = 64;
        const f = new Fenwick(N), arr = new Array(N).fill(0);
        for (let k = 0; k < 1500; k++) {
            const op = (rng() * 3) | 0;
            if (op === 0) {                       /* update */
                const i = (rng() * N) | 0, delta = (rng() * 200 - 100) | 0;
                f.update(i, delta); arr[i] += delta;
            } else if (op === 1) {                /* prefixSum */
                const i = (rng() * N) | 0;
                let s = 0; for (let j = 0; j <= i; j++) s += arr[j];
                assert(f.prefixSum(i) === s, "prefixSum agrees @" + k);
            } else {                              /* rangeQuery */
                let lo = (rng() * N) | 0, hi = (rng() * N) | 0;
                let s = 0; for (let j = lo; j <= hi; j++) s += arr[j];
                assert(f.rangeQuery(lo, hi) === s, "rangeQuery agrees @" + k);
            }
        }
    }
}

/* =====================================================================
 * RingBuffer
 * ===================================================================== */

/* ---------------- fill / overwrite-oldest / get / capacity ---------------- */
{
    throws(() => new RingBuffer(0), "capacity 0 throws RangeError");
    throws(() => new RingBuffer(), "missing capacity throws");
    const r = new RingBuffer(3);
        assert(r.capacity === 3, "capacity");
        assert(r.length === 0 && !r.full, "empty");
        r.push(1); r.push(2);
        assert(eqArr(r.toArray(), [1, 2]) && !r.full, "partial fill");
        r.push(3);
        assert(r.full && eqArr(r.toArray(), [1, 2, 3]), "full at capacity");
        r.push(4);                              /* evicts oldest (1) */
        assert(eqArr(r.toArray(), [2, 3, 4]), "overwrite oldest");
        r.push(5); r.push(6);                   /* evicts 2, 3 */
        assert(eqArr(r.toArray(), [4, 5, 6]), "keeps most recent capacity items");
        assert(r.get(0) === 4 && r.get(2) === 6, "get oldest..newest");
        assert(r.get(3) === undefined, "get past count undefined");
        assert(r.length === 3, "length capped at capacity");
}

/* ---------------- stores objects; identity preserved through wrap -------- */
{
    const r = new RingBuffer(2);
        const o = { k: 1 };
        r.push(o); r.push("s"); r.push(3.5);    /* o evicted */
        assert(r.get(0) === "s" && r.get(1) === 3.5, "post-evict contents");
}

/* ---------------- RingBuffer differential vs a JS array window ----------- */
{
    const rng = mulberry32(53);
    for (let seed = 0; seed < 6; seed++) {
        const cap = 1 + ((rng() * 8) | 0);
        const r = new RingBuffer(cap), ref = [];
            for (let k = 0; k < 3000; k++) {
                const x = (rng() * 1000) | 0;
                r.push(x); ref.push(x);
                if (ref.length > cap) ref.shift();  /* oracle: last `cap` items */
            }
            assert(eqArr(r.toArray(), ref), "ring window agrees @seed " + seed);
            assert(r.length === ref.length, "ring length agrees");
    }
}

/* =====================================================================
 * SegTree
 * ===================================================================== */

/* ---------------- sum / min / max ops ---------------- */
{
    throws(() => new SegTree(0), "size 0 throws");
    throws(() => new SegTree(4, "bogus"), "bad op throws");
    const s = new SegTree(8);                    /* default sum */
        assert(s.size === 8, "size");
        assert(s.rangeQuery(0, 7) === 0, "sum identity is 0");
        s.update(0, 5).update(3, 2).update(7, 10);
        assert(s.rangeQuery(0, 7) === 17, "full sum");
        assert(s.rangeQuery(1, 3) === 2, "partial sum");
        assert(s.rangeQuery(3, 3) === 2, "single-index");
        assert(s.rangeQuery(4, 6) === 0, "empty region");
        assert(s.rangeQuery(5, 2) === 0, "lo>hi -> identity");
        s.update(0, 1);                          /* reassign a leaf */
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
}

/* ---------------- SegTree differential vs a recomputed fold ---------------- */
{
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
}

/* =====================================================================
 * BloomFilter
 * ===================================================================== */

/* ---------------- no false negatives + bounded false positives ---------- */
{
    throws(() => new BloomFilter(0), "0 bits throws");
    const bf = new BloomFilter(4096, 5);
        assert(bf.bits === 4096 && bf.hashes === 5, "params");
        const present = [];
        const rng = mulberry32(71);
        for (let i = 0; i < 300; i++) {
            const key = "k" + ((rng() * 1e9) | 0);
            present.push(key); bf.add(key);
        }
        /* INVARIANT: every added key must test positive (no false negatives) */
        for (const key of present)
            assert(bf.mayContain(key) === true, "no false negative for " + key);
        /* false-positive rate over fresh absentees stays reasonable */
        let fp = 0, trials = 2000;
        for (let i = 0; i < trials; i++)
            if (bf.mayContain("absent#" + i)) fp++;
        assert(fp / trials < 0.15, "false-positive rate bounded: " + (fp / trials));
}

/* ---------------- add is chainable; empty key handled ---------------- */
{
    const bf = new BloomFilter(256);
        assert(bf.add("x") === bf, "add returns the filter");
        bf.add("");
        assert(bf.mayContain("") === true, "empty key round-trips");
        assert(bf.hashes === 3, "default hashes = 3");
}

/* =====================================================================
 * Trie
 * ===================================================================== */

/* ---------------- insert / has / delete / prefixes ---------------- */
{
    const t = new Trie();
        assert(t.size === 0 && !t.has("x"), "empty trie");
        assert(t.insert("cat") === t, "insert returns the trie");
        t.insert("car").insert("card").insert("dog").insert("do");
        assert(t.size === 5, "size after 5 inserts");
        assert(t.has("car") && t.has("card") && t.has("do"), "has stored keys");
        assert(!t.has("ca") && !t.has("care"), "prefix/extension are not members");
        t.insert("cat");
        assert(t.size === 5, "re-insert does not grow size");
        /* longestPrefix: the longest STORED key that prefixes the argument */
        assert(t.longestPrefix("cardance") === "card", "longestPrefix card");
        assert(t.longestPrefix("doggo") === "dog", "longestPrefix dog (> do)");
        assert(t.longestPrefix("xyz") === "", "no prefix -> empty string");
        assert(t.longestPrefix("ca") === "", "prefix present but not a key");
        /* keysWithPrefix (order not guaranteed -> sort) */
        assert(eqArr(t.keysWithPrefix("car").sort(), ["car", "card"]), "keysWithPrefix car");
        assert(eqArr(t.keysWithPrefix("do").sort(), ["do", "dog"]), "keysWithPrefix do");
        assert(eqArr(t.keysWithPrefix("").sort(), ["car", "card", "cat", "do", "dog"]),
               "empty prefix -> all keys");
        assert(eqArr(t.keysWithPrefix("z"), []), "no match -> []");
        /* delete of an internal node key must not drop its children */
        assert(t.delete("car") === true, "delete existing");
        assert(!t.has("car") && t.has("card"), "delete keeps descendant keys");
        assert(t.size === 4, "size drops on delete");
        assert(t.delete("car") === false, "delete missing is false");
}

/* ---------------- empty string as a key ---------------- */
{
    const t = new Trie();
        t.insert("");
        assert(t.has("") && t.size === 1, "empty string is a valid key");
        assert(t.longestPrefix("anything") === "", "empty key is a prefix of all");
        assert(t.keysWithPrefix("").includes(""), "keysWithPrefix('') includes ''");
        t.insert("a");
        assert(eqArr(t.keysWithPrefix("").sort(), ["", "a"]), "empty + 'a'");
}

/* ---------------- deep trie: iterative teardown must not stack-overflow ---- */
{
    (() => {
        const t = new Trie();  /* one 10k-char key => a 10k-deep trie */
        t.insert("a".repeat(10000));
        t.insert("a".repeat(10000) + "b");
        assert(t.has("a".repeat(10000)), "deep key stored");
    })();                       /* t is now unreachable */
    std.gc();                   /* GC frees it via its ITERATIVE finalizer */
    assert(true, "deep trie freed by the GC without a C-stack overflow");
}

/* ---------------- Trie differential vs a JS Set<string> ---------------- */
{
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
        /* keysWithPrefix vs a filter over the reference */
        for (const pfx of ["", "a", "ab", "c", "zz"]) {
            const got = t.keysWithPrefix(pfx).sort();
            const want = [...ref].filter(x => x.startsWith(pfx)).sort();
            assert(eqArr(got, want), "keysWithPrefix('" + pfx + "') agrees");
        }
    }
}

/* =====================================================================
 * LRU
 * ===================================================================== */

/* ---------------- get / put / has / delete / eviction ---------------- */
{
    throws(() => new LRU(0), "capacity 0 throws");
    throws(() => new LRU(), "missing capacity throws");
    const c = new LRU(2);
        assert(c.capacity === 2 && c.size === 0, "empty cache");
        assert(c.get("x") === undefined, "get missing is undefined");
        assert(c.put(1, "a") === c, "put returns the cache (chainable)");
        c.put(2, "b");
        assert(c.size === 2 && c.get(1) === "a" && c.get(2) === "b", "stored");
        c.get(1);                 /* touch 1 => 2 is now LRU */
        c.put(3, "c");            /* evicts 2 */
        assert(!c.has(2), "LRU key evicted");
        assert(c.get(1) === "a" && c.get(3) === "c", "MRU keys kept");
        assert(c.size === 2, "size stays at capacity");
        c.put(1, "A");            /* update existing: value changes, size same */
        assert(c.get(1) === "A" && c.size === 2, "update in place");
        assert(c.delete(3) === true && !c.has(3) && c.size === 1, "delete");
        assert(c.delete(9) === false, "delete missing is false");
        assert(c.has(1) === true, "has does not mutate recency");
}

/* ---------------- capacity 1 ---------------- */
{
    const c = new LRU(1);
        c.put("a", 1); c.put("b", 2);          /* b evicts a */
        assert(!c.has("a") && c.get("b") === 2 && c.size === 1, "capacity 1 evicts");
}

/* ---------------- stores any value; object identity preserved ---------------- */
{
    const c = new LRU(4);
        const o = { k: 1 };
        c.put("o", o); c.put("s", "str"); c.put("n", 3.5);
        assert(c.get("o") === o && c.get("o").k === 1, "object identity");
        assert(c.get("s") === "str" && c.get("n") === 3.5, "mixed values");
}

/* ---------------- LRU differential vs a JS Map recency oracle ---------------- */
{
    const rng = mulberry32(97);
    for (let seed = 0; seed < 6; seed++) {
        const cap = 1 + ((rng() * 6) | 0);
        const c = new LRU(cap);
        const ref = new Map();                 /* Map preserves insertion order;
                                                * we re-insert on access to model MRU */
        const touch = (k, v) => { ref.delete(k); ref.set(k, v); };
        const evictIfNeeded = () => {
            while (ref.size > cap) { const oldest = ref.keys().next().value; ref.delete(oldest); }
        };
        for (let step = 0; step < 3000; step++) {
            const k = "k" + ((rng() * 12) | 0), op = (rng() * 3) | 0;
            if (op === 0) {                     /* put */
                const v = (rng() * 1000) | 0;
                c.put(k, v); touch(k, v); evictIfNeeded();
            } else if (op === 1) {              /* get */
                const got = c.get(k);
                if (ref.has(k)) { const v = ref.get(k); touch(k, v);
                    assert(got === v, "get value agrees @" + step); }
                else assert(got === undefined, "get miss agrees @" + step);
            } else {                            /* has (no recency change) */
                assert(c.has(k) === ref.has(k), "has agrees @" + step);
            }
            assert(c.size === ref.size, "size agrees @" + step);
        }
        /* every surviving key must still be retrievable with the right value */
        for (const [k, v] of ref)
            assert(c.get(k) === v, "surviving key value agrees");
    }
}

/* ---------------- fill / evict / lookup across many rehashes --------------
 * Pins the seeded string hash end to end: thousands of distinct keys walk the
 * growing bucket table (16 -> 8192 buckets), everything inserted is either
 * findable or provably evicted, and eviction walks the recency list in order.
 * The hash itself is not JS-observable (same discipline as dyn-ds.c: in-memory
 * only), so this exercises the structure it feeds rather than the digest. */
{
    const cap = 100, N = 5000;
    const c = new LRU(cap);
    for (let i = 0; i < N; i++) c.put("key:" + i, i * 2);
    assert(c.size === cap, "fill far past capacity keeps exactly the capacity");
    for (let i = 0; i < N - cap; i++)
        assert(!c.has("key:" + i), "early key " + i + " was evicted");
    for (let i = N - cap; i < N; i++)
        assert(c.get("key:" + i) === i * 2, "survivor " + i + " value intact");
    /* recency order, MRU first: the ascending get() loop above leaves
     * key:N-1 most recent and key:N-cap least recent */
    const ents = c.entries();
    assert(ents.length === cap, "entries() sees every survivor");
    assert(ents[0][0] === "key:" + (N - 1) && ents[0][1] === (N - 1) * 2,
           "entries() starts at the MRU");
    assert(ents[cap - 1][0] === "key:" + (N - cap),
           "entries() ends at the least recent survivor");
    /* eviction continues from that LRU end */
    for (let i = 0; i < 10; i++) c.put("fresh:" + i, i);
    assert(!c.has("key:" + (N - cap)), "the next put evicts the LRU end");
    assert(c.has("key:" + (N - cap + 10)), "and only the LRU end");
    for (let i = 0; i < 10; i++)
        assert(c.get("fresh:" + i) === i, "fresh keys are all found");
}

/* =====================================================================
 * SortedSet (ordered set of numbers, skiplist-backed)
 * ===================================================================== */

/* ---------------- add / order / floor / ceil / range ---------------- */
{
    const s = new SortedSet();
        assert(s.size === 0, "empty");
        assert(s.first() === undefined && s.last() === undefined, "empty first/last");
        assert(s.floor(5) === undefined && s.ceil(5) === undefined, "empty floor/ceil");
        assert(s.add(5) === s, "add returns the set (chainable)");
        for (const x of [1, 9, 3, 7, 1, 3]) s.add(x);   /* dups ignored */
        assert(s.size === 5, "duplicates ignored");
        assert(eqArr(s.toArray(), [1, 3, 5, 7, 9]), "ascending order");
        assert(s.first() === 1 && s.last() === 9, "first/last");
        assert(s.has(7) && !s.has(8), "has");
        /* floor = largest <= x; ceil = smallest >= x */
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
}

/* ---------------- negative / float keys ---------------- */
{
    const s = new SortedSet();
        for (const x of [-2.5, 3.14, -10, 0, 3.14]) s.add(x);
        assert(eqArr(s.toArray(), [-10, -2.5, 0, 3.14]), "negatives + floats ordered");
        assert(s.floor(-3) === -10 && s.ceil(-3) === -2.5, "negative floor/ceil");
}

/* ---------------- SortedSet differential vs a sorted JS array ---------------- */
{
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
                /* floor / ceil oracle */
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
}

/* =====================================================================
 * SortedMap (ordered number->value map, skiplist-backed)
 * ===================================================================== */

/* ---------------- set / get / ordered key ops ---------------- */
{
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
        m.set(20, "B");   /* update: value changes, size same, order same */
        assert(m.get(20) === "B" && m.size === 3, "update in place");
        assert(m.has(10) && !m.has(15), "has");
        const r = m.rangeQuery(10, 20);
        assert(r.length === 2 && r[0][0] === 10 && r[0][1] === "a" &&
               r[1][0] === 20 && r[1][1] === "B", "rangeQuery [k,v] pairs");
        assert(m.delete(10) === true && !m.has(10) && m.size === 2, "delete");
        assert(m.delete(10) === false, "delete missing is false");
    throws(() => new SortedMap().set(NaN, 1), "set NaN key throws");
}

/* ---------------- stores any value; identity preserved ---------------- */
{
    const m = new SortedMap();
        const o = { k: 1 };
        m.set(1, o); m.set(2, "s"); m.set(3, 3.5);
        assert(m.get(1) === o && m.get(1).k === 1, "object identity");
        assert(m.get(2) === "s" && m.get(3) === 3.5, "mixed values");
}

/* ---------------- SortedMap differential vs a JS Map + sorted keys ---------------- */
{
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
}

/* =====================================================================
 * GC lifecycle — these are PLAIN objects (like Map/Set), no resource surface
 * ===================================================================== */

/* No .close()/.closed/[Symbol.dispose] exists on any class. */
{
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
}

/* finalizer reclamation: create many, keep none, gc. ASan validates the native
 * buffers AND every stored JSValue are freed (no leak, no double-free). */
{
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
    std.gc(); /* run finalizers now so ASan attributes any fault here */
}

/* reference-cycle reclamation — PROVES gc_mark. A value-holding structure that
 * (transitively) references itself must still be collected: the cycle collector
 * traces the stored JS values through gc_mark. Without gc_mark these would leak
 * forever (refcounting alone can't break a cycle). */
{
    function cycleRef(make, store) {
        const c = make();
        const o = { back: c };          /* o -> c */
        store(c, o);                    /* c -> o  ==> cycle c <-> o */
        return new WeakRef(c);
    }
    const refs = [
        cycleRef(() => new Deque(), (c, o) => c.pushBack(o)),
        cycleRef(() => new RingBuffer(4), (c, o) => c.push(o)),
        cycleRef(() => new LRU(4), (c, o) => c.put("k", o)),
        cycleRef(() => new SortedMap(), (c, o) => c.set(1, o)),
    ];
    std.gc();
    for (const w of refs)
        assert(w.deref() === undefined,
               "reference cycle through a value-holding structure was reclaimed (gc_mark works)");
}

/* =====================================================================
 * Heap / List (merged in from the former dyna:container module)
 * ===================================================================== */

/* =====================================================================
 * Heap
 * ===================================================================== */

/* ---------------- basic min-heap ordering ---------------- */
{
    const h = new Heap((a, b) => a - b);
        assert(h.size === 0 && h.length === 0, "new Heap is empty");
        assert(h.peek() === undefined, "peek on empty is undefined");
        assert(h.pop() === undefined, "pop on empty is undefined");
        for (const v of [5, 3, 8, 1, 9, 2, 7, 4, 6, 0])
            h.push(v);
        assert(h.size === 10, "size after 10 pushes");
        const out = [];
        while (h.size > 0) out.push(h.pop());
        assert(eqArr(out, [0, 1, 2, 3, 4, 5, 6, 7, 8, 9]),
               "pop drains in ascending order: " + JSON.stringify(out));
        assert(h.size === 0, "empty after draining");
}

/* ---------------- max-heap via a flipped comparator ---------------- */
{
    const h = new Heap((a, b) => b - a);
        for (const v of [5, 3, 8, 1, 9, 2, 7, 4, 6, 0])
            h.push(v);
        const out = [];
        while (h.size > 0) out.push(h.pop());
        assert(eqArr(out, [9, 8, 7, 6, 5, 4, 3, 2, 1, 0]),
               "max-heap pop drains descending: " + JSON.stringify(out));
}

/* ---------------- peek does not remove ---------------- */
{
    const h = new Heap((a, b) => a - b);
        h.push(5); h.push(1); h.push(3);
        assert(h.peek() === 1, "peek returns the min");
        assert(h.size === 3, "peek does not change size");
        assert(h.peek() === 1, "peek is idempotent");
        assert(h.pop() === 1, "pop returns the same min peek saw");
        assert(h.size === 2, "size drops after pop");
}

/* ---------------- heapsort a large random array ---------------- */
{
    const rnd = mulberry32(12345);
    const N = 5000;
    const src = [];
    for (let i = 0; i < N; i++) src.push(Math.floor(rnd() * 1e6));
    const h = new Heap((a, b) => a - b);
        for (const v of src) h.push(v);
        assert(h.size === N, "heapsort: size after bulk push");
        const out = [];
        while (h.size > 0) out.push(h.pop());
        const ref = [...src].sort((a, b) => a - b);
        assert(eqArr(out, ref), "heapsort matches Array.prototype.sort");
}

/* ---------------- random interleaved push/pop vs. a reference model ----- */
{
    const rnd = mulberry32(999);
    const h = new Heap((a, b) => a - b);
        for (let round = 0; round < 20; round++) {
            const ref = [];
            const ops = 500;
            for (let i = 0; i < ops; i++) {
                if (ref.length === 0 || rnd() < 0.6) {
                    const v = Math.floor(rnd() * 1e6);
                    h.push(v);
                    ref.push(v);
                } else {
                    ref.sort((a, b) => a - b);
                    const want = ref.shift();
                    const got = h.pop();
                    assert(got === want,
                           "random fuzz round " + round + ": got " + got +
                           " want " + want);
                }
            }
            /* drain whatever remains before the next round */
            ref.sort((a, b) => a - b);
            while (ref.length > 0) {
                const want = ref.shift();
                const got = h.pop();
                assert(got === want, "random fuzz drain round " + round);
            }
            assert(h.size === 0, "heap empty at end of fuzz round " + round);
        }
}

/* ---------------- stores arbitrary JS values, not just numbers --------- */
{
    const h = new Heap((a, b) => a.k - b.k);
        h.push({ k: 3, name: "c" });
        h.push({ k: 1, name: "a" });
        h.push({ k: 2, name: "b" });
        assert(h.pop().name === "a", "object heap pops in key order (a)");
        assert(h.pop().name === "b", "object heap pops in key order (b)");
        assert(h.pop().name === "c", "object heap pops in key order (c)");
}

/* ---------------- the comparator is optional, not permissive ---------- */
{
    /* Omitting it selects natural numeric order, compared in C. Passing
       something that is not a function is still a mistake. */
    const nat = new Heap();
    nat.push(3); nat.push(1); nat.push(2);
    assert(nat.pop() === 1, "Heap() with no comparator is a numeric min-heap");
    throws(() => new Heap(42), "Heap(non-function) throws");
    throws(() => new Heap("nope"), "Heap(string) throws");
    /* and a natural heap refuses a value it cannot order */
    throws(() => { const h = new Heap(); h.push(1); h.push("x"); },
           "a natural heap refuses a non-number");
}

/* ---------------- NaN comparator result treated as 0/equal ------------- */
{
    const h = new Heap(() => NaN);
        h.push(1); h.push(2); h.push(3);
        assert(h.size === 3, "NaN-comparator heap still accepts pushes");
        /* order is unspecified when everything compares "equal", but every
         * element must still come out exactly once and the heap must not
         * corrupt/crash. */
        const out = [h.pop(), h.pop(), h.pop()];
        assert(out.length === 3, "NaN-comparator heap drains fully");
        assert(h.size === 0, "NaN-comparator heap ends empty");
}

/* ---------------- throwing comparator: clean failure, heap stays usable */
{
    const h = new Heap((a, b) => a - b);
        h.push(1); h.push(2);
        let boom = new Heap((a, b) => { throw new Error("boom"); });
            boom.push(1); /* first element: no comparison, always succeeds */
            throws(() => boom.push(2), "throwing comparator surfaces during push");
        /* unrelated heap must be entirely unaffected */
        h.push(0);
        assert(h.pop() === 0, "unrelated heap still orders correctly");
        assert(h.pop() === 1, "unrelated heap still orders correctly (2)");
        assert(h.pop() === 2, "unrelated heap still orders correctly (3)");
}

/* ---------------- throwing comparator on the SAME heap: stays usable --- */
{
    /* Documented behavior: push() appends and increments size BEFORE any
     * comparator call, so a comparator that throws mid-sift leaves its value
     * counted and present (never lost, never duplicated by the container
     * itself) but possibly not fully bubbled to its ideal position -- the
     * same "partially completed, not corrupted" contract Array.prototype.sort
     * has for a throwing comparator. The one thing NOT guaranteed afterward
     * is that the very next pop() sequence is perfectly ascending; what IS
     * guaranteed is that every element is still there exactly once (no
     * memory corruption, no lost/duplicated bookkeeping) and later
     * operations keep working. */
    let shouldThrow = false;
    const h = new Heap((a, b) => {
        if (shouldThrow) throw new Error("boom");
        return a - b;
    });
        h.push(5); h.push(1); h.push(3);
        assert(h.size === 3, "size is 3 before the throwing push");
        shouldThrow = true;
        throws(() => h.push(2), "mid-life throwing comparator surfaces");
        shouldThrow = false;
        assert(h.size === 3,
               "size unchanged after throwing comparator (rolled back)");
        h.push(6);
        const out = [];
        while (h.size > 0) out.push(h.pop());
        const expected = [5, 1, 3, 6].sort((a, b) => a - b);
        assert(eqArr([...out].sort((a, b) => a - b), expected),
               "draining after a throwing comparator loses/duplicates nothing: " +
               JSON.stringify(out));
}

/* ---------------- reentrant mutation guard: comparator pushes/pops ------
 * The `busy` guard still matters (independent of lifetime): a comparator that
 * reenters push()/pop() on the same heap mid-sift must throw, not corrupt the
 * in-progress sift. The heap still orders correctly and loses/duplicates
 * nothing despite the rejected reentrant attempts. */
{
    let h;
    h = new Heap((a, b) => { throws(() => h.push(999), "reentrant push during comparator throws"); return a - b; });
    h.push(3); h.push(1); h.push(2); /* triggers comparator calls */
    assert(h.size === 3, "reentrant push attempts do not corrupt size");
    const out = [];
    while (h.size > 0) out.push(h.pop());
    assert(eqArr(out, [1, 2, 3]), "heap orders correctly despite reentrant-push attempts");

    let h2;
    h2 = new Heap((a, b) => { throws(() => h2.pop(), "reentrant pop during comparator throws"); return a - b; });
    h2.push(3); h2.push(1); h2.push(2);
    const out2 = [];
    while (h2.size > 0) out2.push(h2.pop());
    assert(eqArr(out2, [1, 2, 3]), "heap orders correctly despite reentrant-pop attempts");
}

/* ---------------- Heap is a plain object; finalizer reclaims it -------- */
{
    const h = new Heap((a, b) => a - b);
    assert(h.close === undefined && h.closed === undefined &&
           h[Symbol.dispose] === undefined, "Heap has no resource surface");
    for (let i = 0; i < 500; i++) {
        const hh = new Heap((a, b) => a - b);
        hh.push({ i }); hh.push("s" + i); hh.push(i);   /* never closed */
    }
    std.gc();  /* ASan-validated: values + comparator freed */
}

/* =====================================================================
 * List
 * ===================================================================== */

/* ---------------- basic push/pop both ends + front/back ---------------- */
{
    const l = new List();
        assert(l.length === 0, "new List is empty");
        assert(l.front() === undefined, "front on empty is undefined");
        assert(l.back() === undefined, "back on empty is undefined");
        assert(l.popFront() === undefined, "popFront on empty is undefined");
        assert(l.popBack() === undefined, "popBack on empty is undefined");

        assert(l.pushBack(2) === 1, "pushBack returns new length 1");
        assert(l.pushFront(1) === 2, "pushFront returns new length 2");
        assert(l.pushBack(3) === 3, "pushBack returns new length 3");
        /* list is now [1, 2, 3] */
        assert(l.length === 3, "length is 3");
        assert(l.front() === 1, "front is 1");
        assert(l.back() === 3, "back is 3");
        assert(eqArr(l.toArray(), [1, 2, 3]), "toArray is [1,2,3]");

        assert(l.popFront() === 1, "popFront returns 1");
        assert(l.popBack() === 3, "popBack returns 3");
        assert(l.length === 1, "length is 1 after both pops");
        assert(l.front() === 2 && l.back() === 2, "single element is both front and back");
        assert(l.popFront() === 2, "final popFront returns 2");
        assert(l.length === 0, "empty after draining");
}

/* ---------------- pushFront/pushBack interleaving builds correct order - */
{
    const l = new List();
        l.pushBack(1);      // [1]
        l.pushFront(0);     // [0,1]
        l.pushBack(2);      // [0,1,2]
        l.pushFront(-1);    // [-1,0,1,2]
        l.pushBack(3);      // [-1,0,1,2,3]
        assert(eqArr(l.toArray(), [-1, 0, 1, 2, 3]), "interleaved push order");
        assert(l.length === 5, "length matches after interleaving");
}

/* ---------------- drains identically from both ends -------------------- */
{
    const l = new List();
        const N = 2000;
        for (let i = 0; i < N; i++) l.pushBack(i);
        assert(l.length === N, "length after bulk pushBack");
        assert(eqArr(l.toArray(), Array.from({ length: N }, (_, i) => i)),
               "bulk toArray matches ascending sequence");
        for (let i = 0; i < N; i++) assert(l.popFront() === i, "popFront drains ascending @" + i);
        assert(l.length === 0, "empty after draining forward");

        for (let i = 0; i < N; i++) l.pushFront(i);
        for (let i = 0; i < N; i++) assert(l.popFront() === N - 1 - i, "popFront drains reversed @" + i);
        assert(l.length === 0, "empty after draining reversed");
}

/* ---------------- stores objects/strings/numbers, identity preserved --- */
{
    const l = new List();
        const obj = { tag: "x" };
        l.pushBack(obj); l.pushBack("str"); l.pushBack(4.5);
        assert(l.toArray()[0] === obj, "object identity preserved in toArray");
        assert(l.popFront() === obj, "popFront returns the same object");
        assert(l.popFront() === "str", "popFront returns the string");
        assert(l.popFront() === 4.5, "popFront returns the number");
}

/* ---------------- iteration via for..of ---------------- */
{
    const l = new List();
        l.pushBack(1); l.pushBack(2); l.pushBack(3);
        const seen = [];
        for (const v of l) seen.push(v);
        assert(eqArr(seen, [1, 2, 3]), "for..of visits in list order");
        assert(eqArr([...l], [1, 2, 3]), "spread operator works via [Symbol.iterator]");
        /* iterating does not mutate the list */
        assert(l.length === 3, "length unaffected by iteration");

    /* iterating an empty list yields nothing */
    const l2 = new List();
        const seen2 = [];
        for (const v of l2) seen2.push(v);
        assert(seen2.length === 0, "for..of over empty list yields nothing");
}

/* ---------------- List is a plain object; finalizer reclaims it -------- */
{
    const l = new List();
    assert(l.close === undefined && l.closed === undefined &&
           l[Symbol.dispose] === undefined, "List has no resource surface");
    for (let i = 0; i < 500; i++) {
        const ll = new List();
        ll.pushBack({ i }); ll.pushFront("s" + i); ll.pushBack(i);  /* never closed */
    }
    std.gc();
}

print("test_structures: all tests passed (" + n + " assertions)");
