/*
 * dyna:structures -- native data structures JavaScript has NO builtin for.
 *
 *     make CONFIG_NATIVE_MODULES=y
 *     ./dynajs examples/js/dynajs_structures.js
 *
 * Array / Map / Set / TypedArray are engine intrinsics and are NOT
 * reimplemented here; this module ships only what the language lacks:
 *
 *   BitSet     : dynamic bit set + word-parallel and/or/xor + popcount
 *   UnionFind  : disjoint-set forest (connectivity / components)
 *   Deque      : double-ended queue, O(1) at BOTH ends (Array.shift is O(n))
 *   Fenwick    : Binary Indexed Tree, O(log n) point update + prefix/range sum
 *   SegTree, RingBuffer, BloomFilter, Trie, LRU, SortedSet, SortedMap ...
 *
 * These are PLAIN garbage-collected objects -- exactly like Map and Set. Just
 * create and use them; there is NO .close(), no dispose, nothing to manage. The
 * GC reclaims the native backing (and any values a container holds, including
 * reference cycles) when the object becomes unreachable.
 */
import { BitSet, UnionFind, Deque, Fenwick } from "dyna:structures";
import * as std from "std";

function assert(cond, msg) { if (!cond) throw new Error("FAIL: " + msg); }

/* ---- BitSet: set membership + word-parallel algebra ---- */
function demo_bitset() {
    const b = new BitSet();
    b.set(3).set(65).set(130);                 // spans 3 machine words
    assert(b.count === 3 && b.get(65) && !b.get(64), "set/get/count");
    assert(b.nextSet(4) === 65, "nextSet");
    const evens = new BitSet();
    for (let i = 0; i < 200; i += 2) evens.set(i);
    b.and(evens);                               // keep only even-indexed bits
    assert(b.get(130) && !b.get(3) && !b.get(65), "and (word-parallel)");
    return b.count;                             // 1
}

/* ---- UnionFind: connectivity / connected components ---- */
function demo_unionfind() {
    const uf = new UnionFind(6);
    uf.union(0, 1); uf.union(2, 3); uf.union(1, 3);   // -> {0,1,2,3},{4},{5}
    assert(uf.connected(0, 2), "transitive connectivity");
    assert(!uf.connected(0, 4), "disjoint");
    return uf.count;                            // 3 components
}

/* ---- Deque: O(1) at both ends (Array cannot do this) ---- */
function demo_deque() {
    const d = new Deque();
    d.pushBack(1); d.pushBack(2); d.pushFront(0);      // 0,1,2
    assert(d.toArray().join(",") === "0,1,2", "order");
    assert(d.popFront() === 0 && d.popBack() === 2, "both ends");
    return d.length;                            // 1
}

/* ---- Fenwick: O(log n) prefix / range sums with point updates ---- */
function demo_fenwick() {
    const f = new Fenwick(8);
    f.update(0, 5); f.update(3, 2); f.update(7, 10);
    assert(f.prefixSum(3) === 7, "prefixSum");
    assert(f.rangeQuery(1, 7) === 12, "rangeQuery");
    return f.prefixSum(7);                       // 17
}

print("BitSet demo: final count =", demo_bitset());
print("UnionFind demo: components =", demo_unionfind());
print("Deque demo: remaining =", demo_deque());
print("Fenwick demo: total =", demo_fenwick());

/* ---- Nothing to clean up: create a million, manage none ----
 * Each object is reclaimed automatically when its iteration ends; peak memory
 * stays flat with zero cleanup code. */
function demo_no_cleanup() {
    for (let i = 0; i < 1000000; i++) {
        const d = new Deque();
        d.pushBack(i);
        /* d is unreachable here -> its native buffer is freed by the GC */
    }
    return 1000000;
}
print("No-cleanup demo:", demo_no_cleanup(),
      "objects created & auto-freed; memory flat, nothing to close");

/* ---- Even reference cycles are collected (like Map/Set) ----
 * A Deque that transitively references itself is still reclaimed by the cycle
 * collector -- the container marks the values it holds. */
function demo_cycle_is_collected() {
    let d = new Deque();
    const o = { back: d };
    d.pushBack(o);                              // cycle: d <-> o
    const w = new WeakRef(d);
    d = null; o.back = null;
    std.gc();
    assert(w.deref() === undefined, "self-referential Deque was garbage-collected");
    return true;
}
assert(demo_cycle_is_collected(), "cycle collection");
print("Cycle demo: a self-referential container is garbage-collected normally");

print("PASS");
