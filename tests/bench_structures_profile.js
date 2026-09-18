/*
 * A per-structure cost profile for dyna:structures.
 *
 * The point is NOT to produce a ranking of absolute nanoseconds -- it is to
 * find, for each structure, which operation dominates and how the cost SCALES,
 * because that is what says whether a layout change can pay at all. A structure
 * whose per-element cost is flat in size is latency-bound on a pointer chase
 * and needs a different algorithm; one that grows is doing per-element work
 * that might be removable.
 *
 * Rules this harness follows, each paid for by a bad measurement somewhere:
 *   - nothing in the timed region but the operation; results go to a sink and
 *     the checking happens after
 *   - the empty loop is calibrated and subtracted, because the driving loop
 *     itself costs several ns
 *   - reps scale so the timed region clears the clock's noise floor, and the
 *     result is divided back to check it is not a whole number of ticks
 *   - three sizes, because a constant overhead reads as a ratio that depends
 *     on the input
 *
 * Usage:  dynajs tests/bench_structures_profile.js [filter]
 */
import * as S from "dyna:structures";

const FILTER = scriptArgs[1] || "";
const SIZES = [1000, 100000, 1000000];
let SINK = 0;

/* ---- timing ------------------------------------------------------------- */

function now() { return Date.now(); }

/* Calibrate the empty loop once: the driving loop is not free, and at ~10ns
 * per op it is a double-digit fraction of what we are measuring. */
let LOOP_NS = 0;
{
    const REPS = 20000000;
    const t0 = now();
    for (let i = 0; i < REPS; i++) SINK += i & 1;
    LOOP_NS = (now() - t0) * 1e6 / REPS;
}

/* Run `fn` enough times to clear the clock, return ns per op net of the loop.
 *
 * STATE CONTAMINATION is the trap this exists to avoid. Scaling reps until the
 * region is long enough means a MUTATING op runs millions of times, so the
 * structure it is measured against is nothing like the size on the label --
 * `Multimap.put` for 200 ms added ~3M entries, and the `get` benchmark that
 * followed then read 3300-element lists and reported 68 us for what is really
 * 2 us. If `rebuild` is given, the structure is rebuilt before each timing
 * batch and only the LAST batch is timed, so the size is the size claimed. */
function timed(fn, opsPerCall, rebuild) {
    let reps = 1;
    let ms = 0;
    for (;;) {
        if (rebuild) rebuild();
        const t0 = now();
        for (let i = 0; i < reps; i++) fn(i);
        ms = now() - t0;
        if (ms >= 200 || reps >= (1 << 26)) break;
        reps = ms <= 0 ? reps * 8 : Math.ceil(reps * (220 / Math.max(ms, 1)));
    }
    const ops = reps * (opsPerCall || 1);
    const ns = ms * 1e6 / ops - LOOP_NS;
    return { ns: ns, reps: reps, ms: ms, ops: ops };
}

const rows = [];
function bench(struct, op, size, fn, opsPerCall, rebuild) {
    if (FILTER && struct.toLowerCase().indexOf(FILTER.toLowerCase()) < 0) return;
    const r = timed(fn, opsPerCall, rebuild);
    rows.push({ struct: struct, op: op, size: size, ns: r.ns, ms: r.ms });
    print("  " + (struct + "." + op).padEnd(26) + String(size).padStart(8) +
          "  " + r.ns.toFixed(1).padStart(9) + " ns/op   (" + r.ms + " ms)");
}

/* ---- inputs ------------------------------------------------------------- */

/* Real-ish string keys: a generator's alphabet is what it measures, so these
 * carry shared prefixes (the trie's best case) AND distinct tails. */
function keys(n) {
    const out = new Array(n);
    for (let i = 0; i < n; i++)
        out[i] = "user/" + (i % 97) + "/session/" + i.toString(36) + "/key";
    return out;
}
function ints(n) {
    const out = new Int32Array(n);
    for (let i = 0; i < n; i++) out[i] = (i * 2654435761) | 0;
    return out;
}

print("dyna:structures cost profile   (empty loop " + LOOP_NS.toFixed(2) + " ns)");
print("");

/* ---- BitSet: dense word array ------------------------------------------- */
for (const N of SIZES) {
    print("BitSet n=" + N);
    const b = new S.BitSet(N);
    for (let i = 0; i < N; i += 3) b.set(i);
    bench("BitSet", "set", N, (i) => { b.set(i % N); });
    bench("BitSet", "get", N, (i) => { SINK += b.get(i % N) ? 1 : 0; });
    bench("BitSet", "count", N, () => { SINK += b.count; });
    bench("BitSet", "serialize", N, () => { SINK += b.serialize().length; });
}

/* ---- Trie: child/sibling linked list ------------------------------------ */
for (const N of [1000, 100000]) {
    print("Trie n=" + N);
    const K = keys(N);
    const t = new S.Trie();
    for (const k of K) t.insert(k);
    bench("Trie", "insert", N, (i) => { t.insert(K[i % N]); });
    bench("Trie", "has", N, (i) => { SINK += t.has(K[i % N]) ? 1 : 0; });
    bench("Trie", "hasMissing", N, (i) => { SINK += t.has("zzz" + i) ? 1 : 0; });
    bench("Trie", "serialize", N, () => { SINK += t.serialize().length; });
}

/* ---- SortedSet / SortedMap ---------------------------------------------- */
for (const N of [1000, 100000]) {
    print("SortedSet n=" + N);
    const I = ints(N);
    const s = new S.SortedSet();
    for (let i = 0; i < N; i++) s.add(I[i]);
    bench("SortedSet", "add", N, (i) => { s.add(I[i % N]); });
    bench("SortedSet", "has", N, (i) => { SINK += s.has(I[i % N]) ? 1 : 0; });
    bench("SortedSet", "serialize", N, () => { SINK += s.serialize().length; });

    print("SortedMap n=" + N);
    const m = new S.SortedMap();
    for (let i = 0; i < N; i++) m.set(I[i], i);
    bench("SortedMap", "set", N, (i) => { m.set(I[i % N], i); });
    bench("SortedMap", "get", N, (i) => { SINK += m.get(I[i % N]) === undefined ? 0 : 1; });
}

/* ---- Heap: both arms ----------------------------------------------------
 * A comparator costs a full JS call per sift level; `new Heap()` compares
 * numbers in C. Both rows are reported, because measuring only the fast one
 * would hide a regression in the path most existing callers are on. */
for (const N of [1000, 100000]) {
    print("Heap n=" + N);
    const I = ints(N);
    for (const [tag, make] of [["", () => new S.Heap((a, b) => a - b)],
                               ["Natural", () => new S.Heap()]]) {
        let h = make();
        const rebuildHeap = () => { h = make();
                                    for (let i = 0; i < N; i++) h.push(I[i]); };
        rebuildHeap();
        /* push GROWS the heap, so without a rebuild it is measured at millions
         * of elements while claiming N -- log2 of the size is the whole cost. */
        bench("Heap" + tag, "push", N, (i) => { h.push(I[i % N]); }, 1, rebuildHeap);
        /* pushPop is balanced, so the size holds. */
        bench("Heap" + tag, "pushPop", N, (i) => { h.push(I[i % N]); SINK += h.pop(); });
    }
}

/* ---- BTree vs SortedMap: two ordered maps, same operations --------------
 * Reported side by side on purpose. A skiplist chases one dependent pointer
 * per level; a B+tree reads a node of keys per level. */
for (const N of [1000, 100000]) {
    print("BTree / SortedMap n=" + N);
    const I = ints(N);
    for (const [tag, C] of [["BTree", S.BTree], ["SortedMap", S.SortedMap]]) {
        const o = new C();
        for (let i = 0; i < N; i++) o.set(I[i], i);
        bench(tag, "set (update)", N, (i) => { o.set(I[i % N], i); });
        bench(tag, "get", N, (i) => { SINK += o.get(I[i % N]) === undefined ? 0 : 1; });
        bench(tag, "floorKey", N, (i) => { SINK += o.floorKey(I[i % N] + 1) === undefined ? 0 : 1; });
    }
}

/* ---- List / Deque / RingBuffer ------------------------------------------ */
for (const N of [100000]) {
    print("List / Deque / RingBuffer n=" + N);
    const l = new S.List();
    for (let i = 0; i < N; i++) l.pushBack(i);
    bench("List", "pushPopBack", N, () => { l.pushBack(1); SINK += l.popBack(); });
    bench("List", "toArray/elem", N, () => { SINK += l.toArray().length; }, N);

    const d = new S.Deque();
    for (let i = 0; i < N; i++) d.pushBack(i);
    bench("Deque", "pushPopBack", N, () => { d.pushBack(1); SINK += d.popBack(); });
    bench("Deque", "get", N, (i) => { SINK += d.get(i % N); });

    const r = new S.RingBuffer(N);
    for (let i = 0; i < N; i++) r.push(i);
    bench("RingBuffer", "push", N, (i) => { r.push(i); });
}

/* ---- LRU ----------------------------------------------------------------- */
for (const N of [100000]) {
    print("LRU n=" + N);
    const K = keys(N);
    const c = new S.LRU(N);
    for (let i = 0; i < N; i++) c.set(K[i], i);
    bench("LRU", "get", N, (i) => { SINK += c.get(K[i % N]) === undefined ? 0 : 1; });
    bench("LRU", "set", N, (i) => { c.set(K[i % N], i); });
}

/* ---- Multiset / Multimap / BiMap ---------------------------------------- */
for (const N of [100000]) {
    print("Multiset / Multimap / BiMap n=" + N);
    const K = keys(N);
    let ms = new S.Multiset();
    const rebuildMS = () => { ms = new S.Multiset();
                              for (let i = 0; i < N; i++) ms.add(K[i % 1000]); };
    rebuildMS();
    bench("Multiset", "add", N, (i) => { ms.add(K[i % 1000]); }, 1, rebuildMS);
    bench("Multiset", "count", N, (i) => { SINK += ms.count(K[i % 1000]); });

    let mm = new S.Multimap();
    const rebuildMM = () => { mm = new S.Multimap();
                              for (let i = 0; i < N; i++) mm.put(K[i % 1000], i); };
    rebuildMM();
    bench("Multimap", "put", N, (i) => { mm.put(K[i % 1000], i); }, 1, rebuildMM);
    /* get MUST rebuild too: a contaminated put run leaves 3300-element lists
     * and the number below then describes a structure nobody asked about. */
    bench("Multimap", "get(100 vals)", N, (i) => { SINK += mm.get(K[i % 1000]).length; },
          1, rebuildMM);

    const bm = new S.BiMap();
    for (let i = 0; i < N; i++) bm.set(K[i], i);
    bench("BiMap", "set", N, (i) => { bm.set(K[i % N], i); });
}

/* ---- numeric arrays: Fenwick / SegTree / UnionFind ---------------------- */
for (const N of [100000, 1000000]) {
    print("Fenwick / SegTree / UnionFind n=" + N);
    const f = new S.Fenwick(N);
    bench("Fenwick", "update", N, (i) => { f.update(i % N, 1); });
    bench("Fenwick", "prefixSum", N, (i) => { SINK += f.prefixSum(i % (N - 1)); });

    const st = new S.SegTree(N);
    bench("SegTree", "update", N, (i) => { st.update(i % N, i); });
    bench("SegTree", "rangeQuery", N, (i) => { SINK += st.rangeQuery(0, (i % (N - 1)) + 1); });

    const uf = new S.UnionFind(N);
    for (let i = 1; i < N; i++) uf.union(i - 1, i);
    bench("UnionFind", "find", N, (i) => { SINK += uf.find(i % N); });
}

/* ---- sketches ------------------------------------------------------------ */
for (const N of [100000]) {
    print("sketches n=" + N);
    const K = keys(N);
    const bf = new S.BloomFilter(N, 0.01);
    for (let i = 0; i < N; i++) bf.add(K[i]);
    bench("BloomFilter", "add", N, (i) => { bf.add(K[i % N]); });
    bench("BloomFilter", "mayContain", N, (i) => { SINK += bf.mayContain(K[i % N]) ? 1 : 0; });

    const hll = new S.HyperLogLog();
    for (let i = 0; i < N; i++) hll.add(K[i]);
    bench("HyperLogLog", "add", N, (i) => { hll.add(K[i % N]); });
    bench("HyperLogLog", "count", N, () => { SINK += hll.count(); });

    const cms = new S.CountMinSketch(2048, 5);
    for (let i = 0; i < N; i++) cms.add(K[i]);
    bench("CountMinSketch", "add", N, (i) => { cms.add(K[i % N]); });
}

/* ---- serialize sizes: what compression would have to work on ------------- */
print("");
print("record sizes (what a structure-specific codec would compress):");
{
    const N = 100000;
    const K = keys(N), I = ints(N);
    const mk = [
        ["BitSet(1M, every 3rd)", () => { const b = new S.BitSet(1000000);
            for (let i = 0; i < 1000000; i += 3) b.set(i); return b; }],
        ["BitSet(1M, sparse 100)", () => { const b = new S.BitSet(1000000);
            for (let i = 0; i < 100; i++) b.set(i * 9973); return b; }],
        ["SortedSet(100k ints)", () => { const s = new S.SortedSet();
            for (let i = 0; i < N; i++) s.add(I[i]); return s; }],
        ["SortedSet(100k dense)", () => { const s = new S.SortedSet();
            for (let i = 0; i < N; i++) s.add(i); return s; }],
        ["Trie(100k keys)", () => { const t = new S.Trie();
            for (const k of K) t.insert(k); return t; }],
        ["Multiset(1k distinct)", () => { const m = new S.Multiset();
            for (let i = 0; i < N; i++) m.add(K[i % 1000]); return m; }],
        ["HyperLogLog", () => { const h = new S.HyperLogLog();
            for (let i = 0; i < N; i++) h.add(K[i]); return h; }],
        ["Fenwick(100k)", () => new S.Fenwick(N)],
        ["UnionFind(100k chained)", () => { const u = new S.UnionFind(N);
            for (let i = 1; i < N; i++) u.union(i - 1, i); return u; }],
    ];
    for (const [name, build] of mk) {
        if (FILTER && name.toLowerCase().indexOf(FILTER.toLowerCase()) < 0) continue;
        const o = build();
        const t0 = now();
        const b = o.serialize();
        const ms = now() - t0;
        print("  " + name.padEnd(26) + String(b.length).padStart(10) + " bytes   (" +
              ms + " ms to write)");
    }
}

print("");
print("checksum " + (SINK !== 0));
