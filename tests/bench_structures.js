/* bench_structures.js -- the W1.6 gate (STDLIB_OOP_PLAN 2.2).
 *
 * Lifting the 13 containers into a pure-C core costs an indirect call on every
 * element that the binding used to inline. The plan admits the risk up front and
 * makes the extraction conditional on this file: <= 2% on Heap / Deque / LRU,
 * measured, or the specialised path stays.
 *
 * Construction discipline (CLAUDE.md section 3):
 *  - only the operation is inside the timed region; the container is built in an
 *    untimed setup that reruns per repetition;
 *  - a driving JS `for` loop costs 4-8 ns/iteration, so an empty loop of the same
 *    shape is calibrated and subtracted;
 *  - repetition counts put every case past ~25 ms, well clear of the ~1 us
 *    resolution of performance.now();
 *  - machine-readable `#DATA` lines, because scraping pretty output misparses
 *    names that contain digits.
 *
 * Emits `#DATA<TAB>case<TAB>ops<TAB>ns_per_op`.
 */
import {
    BitSet, BloomFilter, Deque, Fenwick, Heap, LRU, List,
    RingBuffer, SegTree, SortedMap, SortedSet, Trie, UnionFind,
} from "dyna:structures";

const TRIALS = 7;

/* Best-of-TRIALS wall time for one call of `body`, in ns. `setup` runs before
 * each trial and is NOT timed. */
function best(setup, body) {
    let b = Infinity;
    for (let t = 0; t < TRIALS; t++) {
        const arg = setup ? setup() : undefined;
        const t0 = performance.now();
        body(arg);
        const dt = performance.now() - t0;
        if (dt < b) b = dt;
    }
    return b * 1e6;
}

/* Cost of the driving loop alone, per iteration, at this iteration count. */
function loopFloor(n) {
    let b = Infinity;
    for (let t = 0; t < TRIALS; t++) {
        const t0 = performance.now();
        let s = 0;
        for (let i = 0; i < n; i++) s += i;
        const dt = performance.now() - t0;
        if (dt < b) b = dt;
        if (s === -1) print("no");
    }
    return b * 1e6 / n;
}

const FLOOR = {};
function floorFor(n) {
    if (FLOOR[n] === undefined) FLOOR[n] = loopFloor(n);
    return FLOOR[n];
}

function bench(name, n, setup, body) {
    const total = best(setup, body);
    const per = total / n - floorFor(n);
    print(`${name.padEnd(28)} ${n.toString().padStart(8)} ops  ${per.toFixed(2).padStart(8)} ns/op`);
    print(`#DATA\t${name}\t${n}\t${per.toFixed(3)}`);
    return per;
}

const N = 200000;

/* ---------- value-holding containers: the ones the vtable would touch ------- */

/* Heap already calls a JS comparator per sift step, so an indirect call in the
 * element ops is noise here by construction -- recorded to prove it. */
bench("Heap.push", N,
      () => new Heap((a, b) => a - b),
      (h) => { for (let i = 0; i < N; i++) h.push((i * 2654435761) >>> 0); });

bench("Heap.push+pop", N,
      () => { const h = new Heap((a, b) => a - b);
              for (let i = 0; i < N; i++) h.push((i * 2654435761) >>> 0);
              return h; },
      (h) => { for (let i = 0; i < N; i++) h.pop(); });

/* Deque and RingBuffer are the risk: their per-element work today is an inlined
 * JS_DupValue / JS_FreeValue with no callback at all. */
bench("Deque.pushBack", N,
      () => new Deque(),
      (d) => { for (let i = 0; i < N; i++) d.pushBack(i); });

bench("Deque.pushBack+popFront", N,
      () => new Deque(),
      (d) => { for (let i = 0; i < N; i++) { d.pushBack(i); d.popFront(); } });

bench("RingBuffer.push", N,
      () => new RingBuffer(1024),
      (r) => { for (let i = 0; i < N; i++) r.push(i); });

bench("List.pushBack+popFront", N,
      () => new List(),
      (l) => { for (let i = 0; i < N; i++) { l.pushBack(i); l.popFront(); } });

/* LRU: hashed byte keys plus a recency-list splice per hit. */
{
    const keys = [];
    for (let i = 0; i < 4096; i++) keys.push("key/" + i);
    bench("LRU.put", N,
          () => new LRU(2048),
          (c) => { for (let i = 0; i < N; i++) c.put(keys[i & 4095], i); });
    bench("LRU.get(hit)", N,
          () => { const c = new LRU(8192);
                  for (let i = 0; i < 4096; i++) c.put(keys[i], i);
                  return c; },
          (c) => { for (let i = 0; i < N; i++) c.get(keys[i & 4095]); });
}

bench("SortedMap.set", 100000,
      () => new SortedMap(),
      (m) => { for (let i = 0; i < 100000; i++) m.set((i * 2654435761) >>> 0, i); });

bench("SortedMap.get", 100000,
      () => { const m = new SortedMap();
              for (let i = 0; i < 100000; i++) m.set((i * 2654435761) >>> 0, i);
              return m; },
      (m) => { for (let i = 0; i < 100000; i++) m.get((i * 2654435761) >>> 0); });

/* ---------- numeric containers: pure C already, move verbatim -------------- */

bench("BitSet.set", N,
      () => new BitSet(N),
      (b) => { for (let i = 0; i < N; i++) b.set(i); });

bench("BitSet.get", N,
      () => { const b = new BitSet(N);
              for (let i = 0; i < N; i += 3) b.set(i);
              return b; },
      (b) => { for (let i = 0; i < N; i++) b.get(i); });

bench("UnionFind.union", N,
      () => new UnionFind(N),
      (u) => { for (let i = 1; i < N; i++) u.union(i - 1, i); });

bench("UnionFind.find", N,
      () => { const u = new UnionFind(N);
              for (let i = 1; i < N; i++) u.union(i - 1, i);
              return u; },
      (u) => { for (let i = 0; i < N; i++) u.find(i); });

bench("Fenwick.update", N,
      () => new Fenwick(N),
      (f) => { for (let i = 0; i < N; i++) f.update(i, 1); });

bench("Fenwick.prefixSum", N,
      () => { const f = new Fenwick(N);
              for (let i = 0; i < N; i++) f.update(i, 1);
              return f; },
      (f) => { for (let i = 0; i < N; i++) f.prefixSum(i); });

bench("SegTree.rangeQuery", N,
      () => { const s = new SegTree(4096, "sum");
              for (let i = 0; i < 4096; i++) s.update(i, i);
              return s; },
      (s) => { for (let i = 0; i < N; i++) s.rangeQuery(i & 2047, 2048 + (i & 2047)); });

bench("BloomFilter.add", N,
      () => new BloomFilter(N, 0.01),
      (b) => { for (let i = 0; i < N; i++) b.add("item/" + (i & 8191)); });

bench("BloomFilter.mayContain", N,
      () => { const b = new BloomFilter(N, 0.01);
              for (let i = 0; i < 8192; i++) b.add("item/" + i);
              return b; },
      (b) => { for (let i = 0; i < N; i++) b.mayContain("item/" + (i & 8191)); });

{
    const words = [];
    for (let i = 0; i < 20000; i++) words.push("prefix/" + i.toString(36) + "/tail");
    bench("Trie.insert", 20000,
          () => new Trie(),
          (t) => { for (let i = 0; i < 20000; i++) t.insert(words[i]); });
    bench("Trie.has", 20000,
          () => { const t = new Trie();
                  for (let i = 0; i < 20000; i++) t.insert(words[i]);
                  return t; },
          (t) => { for (let i = 0; i < 20000; i++) t.has(words[i]); });
}

bench("SortedSet.add", 100000,
      () => new SortedSet(),
      (s) => { for (let i = 0; i < 100000; i++) s.add((i * 2654435761) >>> 0); });

print("");
print("loop floor per iteration: " +
      Object.keys(FLOOR).map(k => `${k}:${FLOOR[k].toFixed(2)}ns`).join("  "));
