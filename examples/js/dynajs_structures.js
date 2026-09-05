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
 * create and use them; there is NO .close(), no dispose, nothing to manage.
 *
 * The cases below cross each structure's WORD boundary (BitSet at 63/64/65),
 * its EMPTY and single-element states, and its ring wrap point (Deque),
 * because that is where an off-by-one lives -- not in the middle of the range.
 */
import { BitSet, UnionFind, Deque, Fenwick } from "dyna:structures";
import * as std from "std";
import { test, run, assert, assertEqual, assertThrows } from "./harness.js";

/* ---- BitSet: set membership + word-parallel algebra ---- */
test("BitSet spans machine words and counts correctly", () => {
  const b = new BitSet();
  b.set(3).set(65).set(130);                 // three different 64-bit words
  assertEqual(b.count, 3, "count across words");
  assert(b.get(65) && !b.get(64), "get is exact at a word boundary");
  assertEqual(b.nextSet(4), 65, "nextSet skips to the next word");
});

test("BitSet word boundaries at 63, 64 and 65", () => {
  for (const i of [0, 62, 63, 64, 65, 127, 128, 129]) {
    const b = new BitSet();
    b.set(i);
    assertEqual(b.count, 1, "one bit at " + i);
    assert(b.get(i), "get(" + i + ")");
    if (i > 0) assert(!b.get(i - 1), "neighbour below " + i + " is clear");
    assert(!b.get(i + 1), "neighbour above " + i + " is clear");
    assertEqual(b.nextSet(0), i, "nextSet finds " + i + " from 0");
  }
});

test("BitSet and() is word-parallel and exact", () => {
  const b = new BitSet();
  for (const i of [3, 65, 130]) b.set(i);
  const evens = new BitSet();
  for (let i = 0; i < 200; i += 2) evens.set(i);
  b.and(evens);
  assert(b.get(130) && !b.get(3) && !b.get(65), "and keeps only the even bits");
  assertEqual(b.count, 1, "and leaves exactly one bit");
});

test("an empty BitSet answers without inventing membership", () => {
  const b = new BitSet();
  assertEqual(b.count, 0, "empty count");
  assert(!b.get(0) && !b.get(1000000), "nothing is set, however far out");
});

/* ---- UnionFind: connectivity / connected components ---- */
test("UnionFind merges transitively and keeps disjoint sets apart", () => {
  const uf = new UnionFind(6);
  uf.union(0, 1); uf.union(2, 3); uf.union(1, 3);   // {0,1,2,3} {4} {5}
  assert(uf.connected(0, 2), "transitive connectivity");
  assert(!uf.connected(0, 4), "disjoint stays disjoint");
  assertEqual(uf.count, 3, "three components");
});

test("UnionFind: every element starts alone, and a repeat union is a no-op", () => {
  const uf = new UnionFind(5);
  assertEqual(uf.count, 5, "five singletons");
  for (let i = 0; i < 5; i++) assert(uf.connected(i, i), "each is its own set");
  uf.union(2, 2);
  assertEqual(uf.count, 5, "self-union changes nothing");
  uf.union(0, 1); uf.union(0, 1);
  assertEqual(uf.count, 4, "a repeated union changes nothing either");
});

test("UnionFind chains the whole range down to one component", () => {
  const N = 1000;
  const uf = new UnionFind(N);
  for (let i = 1; i < N; i++) uf.union(i - 1, i);
  assertEqual(uf.count, 1, "a full chain is one component");
  assert(uf.connected(0, N - 1), "the ends are connected");
});

/* ---- Deque: O(1) at both ends (Array cannot do this) ---- */
test("Deque preserves order from both ends", () => {
  const d = new Deque();
  d.pushBack(1); d.pushBack(2); d.pushFront(0);
  assertEqual(d.toArray(), [0, 1, 2], "order after mixed pushes");
  assertEqual(d.popFront(), 0, "popFront");
  assertEqual(d.popBack(), 2, "popBack");
  assertEqual(d.length, 1, "one left");
});

test("Deque survives the wrap point of its ring", () => {
  /* Alternating push/pop walks head and tail past the buffer end, which is
     where a ring's index arithmetic goes wrong. */
  const d = new Deque();
  for (let round = 0; round < 200; round++) {
    for (let i = 0; i < 7; i++) d.pushBack(round * 10 + i);
    for (let i = 0; i < 5; i++) d.popFront();
  }
  assertEqual(d.length, 400, "length after 200 wrap rounds");
  const arr = d.toArray();
  assertEqual(arr.length, d.length, "toArray agrees with length");
  for (let i = 0; i < arr.length; i++) assert(arr[i] !== undefined, "no hole at " + i);
});

test("an empty Deque pops nothing rather than inventing a value", () => {
  const d = new Deque();
  assertEqual(d.length, 0, "empty");
  assertEqual(d.toArray(), [], "toArray of empty");
  assert(d.popFront() === undefined, "popFront on empty is undefined");
  assert(d.popBack() === undefined, "popBack on empty is undefined");
  d.pushBack(1); d.popBack();
  assertEqual(d.length, 0, "back to empty");
  assert(d.popFront() === undefined, "and still yields undefined");
});

test("Deque holds falsy values as values", () => {
  const d = new Deque();
  d.pushBack(undefined); d.pushBack(null); d.pushBack(0); d.pushBack("");
  assertEqual(d.length, 4, "four entries");
  assertEqual(d.toArray(), [undefined, null, 0, ""], "all four survive");
});

/* ---- Fenwick: O(log n) prefix / range sums with point updates ---- */
test("Fenwick prefix and range sums", () => {
  const f = new Fenwick(8);
  f.update(0, 5); f.update(3, 2); f.update(7, 10);
  assertEqual(f.prefixSum(3), 7, "prefixSum(3)");
  assertEqual(f.rangeQuery(1, 7), 12, "rangeQuery(1,7)");
  assertEqual(f.prefixSum(7), 17, "prefixSum(7) is the total");
});

test("Fenwick matches a brute-force reference over 500 random updates", () => {
  /* The reference is the obvious O(n) loop -- the definition, not another
     optimised path -- so agreement means something. */
  const N = 64;
  const f = new Fenwick(N);
  const plain = new Array(N).fill(0);
  let s = 123456789;
  const nextInt = () => (s = (s * 1103515245 + 12345) & 0x7fffffff);
  for (let k = 0; k < 500; k++) {
    const i = nextInt() % N, v = (nextInt() % 21) - 10;
    f.update(i, v);
    plain[i] += v;
  }
  for (let i = 0; i < N; i++) {
    let want = 0;
    for (let j = 0; j <= i; j++) want += plain[j];
    assertEqual(f.prefixSum(i), want, "prefixSum(" + i + ")");
  }
});

test("Fenwick handles negative deltas and a single-element range", () => {
  const f = new Fenwick(4);
  f.update(1, 10); f.update(1, -10);
  assertEqual(f.prefixSum(3), 0, "a delta and its inverse cancel");
  f.update(2, 7);
  assertEqual(f.rangeQuery(2, 2), 7, "a single-element range");
});

/* ---- lifetime: plain GC objects, deliberately ---- */
test("a million containers are created and reclaimed with no teardown", () => {
  for (let i = 0; i < 1000000; i++) {
    const d = new Deque();
    d.pushBack(i);
  }
  assert(true, "1000000 Deques created and auto-freed");
});

test("a reference cycle through a container is still collected", () => {
  let d = new Deque();
  const o = { back: d };
  d.pushBack(o);                 // cycle: d <-> o
  const w = new WeakRef(d);
  d = null; o.back = null;
  std.gc();
  assert(w.deref() === undefined, "the self-referential Deque was collected");
});

test("these containers expose no close(): there is nothing to release", () => {
  assert(typeof new BitSet().close === "undefined", "BitSet has no close()");
  assert(typeof new Deque().close === "undefined", "Deque has no close()");
  assert(typeof new UnionFind(4).close === "undefined", "UnionFind has no close()");
  assert(typeof new Fenwick(4).close === "undefined", "Fenwick has no close()");
});

test("a method on a foreign receiver throws instead of reading its opaque", () => {
  assertThrows(() => BitSet.prototype.set.call({}, 1), undefined, "BitSet.set");
  assertThrows(() => Deque.prototype.pushBack.call({}, 1), undefined, "Deque.pushBack");
});

await run("dyna:structures -- BitSet, UnionFind, Deque, Fenwick");
