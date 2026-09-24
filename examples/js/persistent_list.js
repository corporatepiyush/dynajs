// persistent_list.js — an immutable persistent vector backed by a
// bit-partitioned trie with a branching factor of 32 (à la Clojure's
// PersistentVector). All updates (push/pop/set) copy only the O(log32 n) nodes
// along one path and structurally share everything else, so old versions remain
// valid and unchanged forever.
//
// Engine features exercised: #private fields, Symbol.iterator generators,
// structural-sharing (a "structured-clone-free" persistent update), and
// property-based testing driven by the harness's seeded PRNG.

import { test, run, assert, assertEqual, assertThrows, rng, randInt } from "./harness.js";

const BITS = 5;
const WIDTH = 1 << BITS; // 32
const MASK = WIDTH - 1; // 31

/**
 * Immutable persistent vector. Nodes are plain arrays treated as read-only by
 * convention; every mutation returns a new PersistentVector.
 */
export class PersistentVector {
  #root; // trie root (array of children, or values at a leaf)
  #size; // element count
  #shift; // bit-shift of the root level = height * BITS

  constructor(root, size, shift) {
    this.#root = root;
    this.#size = size;
    this.#shift = shift;
  }

  static empty() {
    return new PersistentVector([], 0, 0);
  }

  static from(iterable) {
    let v = PersistentVector.empty();
    for (const x of iterable) v = v.push(x);
    return v;
  }

  get size() {
    return this.#size;
  }

  /** Capacity of a tree whose root sits at the given shift. */
  static #capacity(shift) {
    return 1 << (shift + BITS);
  }

  get(index) {
    if (index < 0 || index >= this.#size) {
      throw new RangeError(`index ${index} out of range [0, ${this.#size})`);
    }
    let node = this.#root;
    for (let level = this.#shift; level > 0; level -= BITS) {
      node = node[(index >> level) & MASK];
    }
    return node[index & MASK];
  }

  /** Return a new vector with element `index` replaced by `value`. */
  set(index, value) {
    if (index < 0 || index >= this.#size) {
      throw new RangeError(`index ${index} out of range [0, ${this.#size})`);
    }
    const newRoot = this.#assoc(this.#root, this.#shift, index, value);
    return new PersistentVector(newRoot, this.#size, this.#shift);
  }

  #assoc(node, level, index, value) {
    const copy = node.slice(); // copy just this node (path-copy)
    if (level === 0) {
      copy[index & MASK] = value;
    } else {
      const sub = (index >> level) & MASK;
      copy[sub] = this.#assoc(node[sub], level - BITS, index, value);
    }
    return copy;
  }

  /** Return a new vector with `value` appended. */
  push(value) {
    const size = this.#size;
    if (size === PersistentVector.#capacity(this.#shift)) {
      // Root is full: grow a new level whose first child is the old root.
      const newRoot = [this.#root, PersistentVector.#newPath(this.#shift, value)];
      return new PersistentVector(newRoot, size + 1, this.#shift + BITS);
    }
    const newRoot = this.#pushInto(this.#shift, this.#root, size, value);
    return new PersistentVector(newRoot, size + 1, this.#shift);
  }

  /** Build a chain of single-child nodes ending in a leaf holding `value`. */
  static #newPath(level, value) {
    if (level === 0) return [value];
    return [PersistentVector.#newPath(level - BITS, value)];
  }

  #pushInto(level, node, index, value) {
    const copy = node ? node.slice() : [];
    const sub = (index >> level) & MASK;
    if (level === 0) {
      copy[sub] = value;
    } else {
      const child = node ? node[sub] : undefined;
      copy[sub] = child
        ? this.#pushInto(level - BITS, child, index, value)
        : PersistentVector.#newPath(level - BITS, value);
    }
    return copy;
  }

  /** Return [newVector, removedValue] with the last element removed. */
  pop() {
    if (this.#size === 0) throw new RangeError("pop from empty vector");
    const removed = this.get(this.#size - 1);
    if (this.#size === 1) return [PersistentVector.empty(), removed];

    const newSize = this.#size - 1;
    let newRoot = this.#popTail(this.#shift, this.#root, newSize);
    let newShift = this.#shift;
    // Collapse now-redundant root levels with a single child.
    while (newShift > 0 && newRoot.length === 1) {
      newRoot = newRoot[0];
      newShift -= BITS;
    }
    return [new PersistentVector(newRoot, newSize, newShift), removed];
  }

  #popTail(level, node, newSize) {
    const sub = (newSize >> level) & MASK; // slot of the element being removed
    if (level === 0) {
      if (sub === 0) return null;
      return node.slice(0, sub);
    }
    const child = this.#popTail(level - BITS, node[sub], newSize);
    if (child === null && sub === 0) return null;
    const copy = node.slice(0, sub + 1);
    if (child === null) copy.length = sub; // drop the emptied child
    else copy[sub] = child;
    return copy;
  }

  *[Symbol.iterator]() {
    for (let i = 0; i < this.#size; i++) yield this.get(i);
  }

  toArray() {
    return [...this];
  }
}

// --- tests -------------------------------------------------------------------

test("empty vector basics", () => {
  const v = PersistentVector.empty();
  assertEqual(v.size, 0);
  assertEqual(v.toArray(), []);
  assertThrows(() => v.get(0), "out of range");
  assertThrows(() => v.pop(), "empty");
});

test("push/get across multiple trie levels", () => {
  let v = PersistentVector.empty();
  const N = 2000; // > 32*32 forces a 3-level trie
  for (let i = 0; i < N; i++) v = v.push(i * 3);
  assertEqual(v.size, N);
  for (let i = 0; i < N; i++) assertEqual(v.get(i), i * 3);
});

test("set does not mutate the source (persistence)", () => {
  const base = PersistentVector.from([10, 20, 30, 40]);
  const updated = base.set(2, 99);
  assertEqual(base.toArray(), [10, 20, 30, 40]); // original intact
  assertEqual(updated.toArray(), [10, 20, 99, 40]);
  assertThrows(() => base.set(9, 0), "out of range");
});

test("pop returns value and shrinks, source unchanged", () => {
  const base = PersistentVector.from([1, 2, 3]);
  const [popped, value] = base.pop();
  assertEqual(value, 3);
  assertEqual(popped.toArray(), [1, 2]);
  assertEqual(base.toArray(), [1, 2, 3]); // still intact
});

test("structural sharing: many versions coexist", () => {
  const versions = [PersistentVector.empty()];
  for (let i = 0; i < 500; i++) versions.push(versions[i].push(i));
  // Every historical version still reports exactly its own contents.
  for (let i = 0; i < versions.length; i++) {
    assertEqual(versions[i].size, i);
    if (i > 0) assertEqual(versions[i].get(i - 1), i - 1);
  }
});

test("property test: random ops mirror a reference array", () => {
  const next = rng(0xC0FFEE);
  let vec = PersistentVector.empty();
  const ref = [];
  const snapshots = []; // {vec, copy} to verify immutability of old versions

  for (let step = 0; step < 4000; step++) {
    const roll = next();
    if (roll < 0.55 || ref.length === 0) {
      const x = randInt(next, 0, 1_000_000);
      vec = vec.push(x);
      ref.push(x);
    } else if (roll < 0.8) {
      const i = randInt(next, 0, ref.length - 1);
      const x = randInt(next, 0, 1_000_000);
      vec = vec.set(i, x);
      ref[i] = x;
    } else {
      const [nv] = vec.pop();
      vec = nv;
      ref.pop();
    }

    // Invariant: the live vector always equals the reference array.
    assertEqual(vec.size, ref.length);
    if (step % 137 === 0) {
      assertEqual(vec.toArray(), ref);
      snapshots.push({ vec, copy: ref.slice() });
    }
  }

  // Final equality plus a full-scan get() check.
  assertEqual(vec.toArray(), ref);
  for (let i = 0; i < ref.length; i++) assertEqual(vec.get(i), ref[i]);

  // Immutability: every snapshot still matches the array it was taken with.
  for (const { vec: snap, copy } of snapshots) {
    assertEqual(snap.toArray(), copy);
  }
});

test("iteration protocol and from()", () => {
  const v = PersistentVector.from("abcde");
  assertEqual([...v], ["a", "b", "c", "d", "e"]);
  assertEqual(Array.from(v, (c) => c.toUpperCase()), ["A", "B", "C", "D", "E"]);
});

await run("persistent vector");
