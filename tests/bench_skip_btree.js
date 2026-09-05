/*
 * bench_skip_btree.js -- audit M30-04: can the SkipList family (SortedMap /
 * SortedSet) consolidate onto the B+tree core (dyn_btree) without regressing?
 *
 * Compares, per operation that Skip users actually run:
 *   insert  ascending / descending / random  (ascending is where skiplists and
 *           btrees differ most: btree splits vs skiplist pointer hops)
 *   lookup  hit / miss
 *   near    floor / ceil, first/last
 *   delete  random order, plus churn (delete+reinsert alternating)
 *   range   1% slice, full iteration (keys()/toArray())
 *
 * at n = 1e3 / 1e4 / 1e5 / 1e6. Also prints, per structure, the same rows as
 * #DATA lines (house style, see bench_structures.js).
 *
 * Memory (separate process per measurement, because peakRss never shrinks):
 *   ./dynajs tests/bench_skip_btree.js --mem <Class> <n>
 * reports the nativeSize ledger delta (dyn_nat allocations: the skiplist is
 * counted, the dyn_btree core's plain calloc is NOT) and the peakRss delta.
 *
 * Keys are doubles generated from a seeded PRNG, so every structure sees the
 * identical key streams and reruns are reproducible.
 */
import { SortedMap, SortedSet, BTree } from "dyna:structures";
import * as sys from "dyna:sys";

const TRIALS_SMALL = 5, TRIALS_BIG = 3;

function mulberry32(a) {
    return function () {
        a |= 0; a = (a + 0x6D2B79F5) | 0;
        let t = Math.imul(a ^ (a >>> 15), 1 | a);
        t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
        return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    };
}

function shuffled(n, rnd) {
    const a = new Array(n);
    for (let i = 0; i < n; i++) a[i] = i;
    for (let i = n - 1; i > 0; i--) {
        const j = (rnd() * (i + 1)) | 0;
        const t = a[i]; a[i] = a[j]; a[j] = t;
    }
    return a;
}

const KEYS = {};   /* per-n key streams, shared by all structures */
function keysFor(n) {
    if (KEYS[n]) return KEYS[n];
    const rnd = mulberry32(0xA0D17 + n);
    const asc = new Array(n);
    for (let i = 0; i < n; i++) asc[i] = i;
    const desc = new Array(n);
    for (let i = 0; i < n; i++) desc[i] = n - 1 - i;
    const rand = shuffled(n, mulberry32(0x5EED + n));
    const miss = new Array(n);
    for (let i = 0; i < n; i++) miss[i] = i + 0.5;      /* never present */
    const del = shuffled(n, mulberry32(0xDE1 + n));
    const hit = shuffled(n, mulberry32(0xB17 + n));     /* lookup order */
    KEYS[n] = { asc, desc, rand, miss, del, hit };
    return KEYS[n];
}

function best(setup, body, trials) {
    let b = Infinity;
    for (let t = 0; t < trials; t++) {
        const arg = setup();
        const t0 = performance.now();
        body(arg);
        const dt = performance.now() - t0;
        if (dt < b) b = dt;
    }
    return b * 1e6;   /* ns for the whole body */
}

function loopFloorNs(n) {
    let b = Infinity;
    for (let t = 0; t < 3; t++) {
        const t0 = performance.now();
        let s = 0;
        for (let i = 0; i < n; i++) s += i;
        const dt = performance.now() - t0;
        if (dt < b) b = dt;
        if (s === -1) print("no");
    }
    return b * 1e6;
}

function fmt(x) {
    if (x >= 1e9) return (x / 1e9).toFixed(2) + "s ";
    if (x >= 1e6) return (x / 1e6).toFixed(2) + "ms";
    if (x >= 1e3) return (x / 1e3).toFixed(1) + "us";
    return x.toFixed(0) + "ns";
}

/* One benchmarked case: body runs `ops` operations. Reports and #DATA-prints
 * ns/op net of the driving-loop floor. */
let DATA = [];
function row(label, n, ops, setup, body, trials) {
    const total = best(setup, body, trials);
    const ns = (total - loopFloorNs(ops)) / ops;
    if (ns < 0) return;   /* floor above measurement: report nothing */
    DATA.push("#DATA\t" + label + "\t" + n + "\t" + ops + "\t" + ns.toFixed(2));
}

function trialsFor(n) { return n >= 1e5 ? TRIALS_BIG : TRIALS_SMALL; }

/* ---- map family: SortedMap (skiplist) vs BTree (B+tree) ----------------- */
const CLASSES = { SortedMap, BTree };
function benchMap(name, n) {
    const C = CLASSES[name];
    const k = keysFor(n);
    const trials = trialsFor(n);
    const make = () => new C();

    row(name + ".set asc", n, n, make, (m) => {
        for (let i = 0; i < n; i++) m.set(k.asc[i], i);
    }, trials);
    row(name + ".set desc", n, n, make, (m) => {
        for (let i = 0; i < n; i++) m.set(k.desc[i], i);
    }, trials);
    row(name + ".set rand", n, n, make, (m) => {
        for (let i = 0; i < n; i++) m.set(k.rand[i], i);
    }, trials);

    /* update-in-place on a full structure */
    const full = () => { const m = new C(); for (let i = 0; i < n; i++) m.set(i, i); return m; };
    row(name + ".set update", n, n, full, (m) => {
        for (let i = 0; i < n; i++) m.set(k.hit[i], i);
    }, trials);

    row(name + ".get hit", n, n, full, (m) => {
        let s = 0;
        for (let i = 0; i < n; i++) if (m.get(k.hit[i]) !== undefined) s++;
        if (s === -1) print("no");
    }, trials);
    row(name + ".get miss", n, n, full, (m) => {
        let s = 0;
        for (let i = 0; i < n; i++) if (m.get(k.miss[i]) !== undefined) s++;
        if (s === -1) print("no");
    }, trials);
    row(name + ".has", n, n, full, (m) => {
        let s = 0;
        for (let i = 0; i < n; i++) if (m.has(k.hit[i])) s++;
        if (s === -1) print("no");
    }, trials);
    row(name + ".floorKey", n, n, full, (m) => {
        let s = 0;
        for (let i = 0; i < n; i++) if (m.floorKey(k.miss[i]) !== undefined) s++;
        if (s === -1) print("no");
    }, trials);
    row(name + ".ceilKey", n, n, full, (m) => {
        let s = 0;
        for (let i = 0; i < n; i++) if (m.ceilKey(k.miss[i]) !== undefined) s++;
        if (s === -1) print("no");
    }, trials);

    /* cheap endpoints need many reps to clear the clock */
    const reps = Math.max(1000, Math.min(200000, n));
    const ep = full();
    row(name + ".first+lastKey", n, reps, () => ep, (m) => {
        let s = 0;
        for (let i = 0; i < reps; i++) { if (m.firstKey() !== undefined) s++; if (m.lastKey() !== undefined) s++; }
        if (s === -1) print("no");
    }, trials);

    /* 1% slice in the middle */
    if (n >= 100) {
        const lo = (n / 2) | 0, hi = lo + Math.max(1, (n / 100) | 0);
        const rreps = Math.max(1, Math.min(1000, (200000 / n * 100) | 0) || 1);
        const r = full();
        row(name + ".rangeQuery 1%", n, rreps, () => r, (m) => {
            let s = 0;
            for (let i = 0; i < rreps; i++) s += m.rangeQuery(lo, hi).length;
            if (s === -1) print("no");
        }, trials);
    }

    /* full ordered iteration: keys() (both) and entries() (SortedMap) or
     * rangeQuery() (BTree) */
    const it = full();
    row(name + ".keys()", n, 1, () => it, (m) => {
        const a = m.keys();
        if (a.length !== n) print("BAD keys " + a.length);
    }, trials);
    if (name === "SortedMap") {
        row(name + ".entries()", n, 1, () => it, (m) => {
            const a = m.entries();
            if (a.length !== n) print("BAD entries");
        }, trials);
    } else {
        row(name + ".rangeQuery(all)", n, 1, () => it, (m) => {
            const a = m.rangeQuery();
            if (a.length !== n) print("BAD range");
        }, trials);
    }

    row(name + ".delete rand", n, n, full, (m) => {
        for (let i = 0; i < n; i++) m.delete(k.del[i]);
    }, trials);

    /* churn: alternate delete + reinsert (btree never rebalances on delete) */
    row(name + ".churn del+set", n, n, full, (m) => {
        for (let i = 0; i < n; i++) { const key = k.hit[i]; m.delete(key); m.set(key, i); }
    }, trials);
}

/* ---- set family: SortedSet vs BTree-as-set (value undefined) ------------
 * BTree.set(k, undefined) is exactly what a SortedSet-on-btree would store. */
function benchSet(n) {
    const k = keysFor(n);
    const trials = trialsFor(n);
    const mkS = () => new SortedSet();
    const mkB = () => new BTree();

    const rows = [
        ["SortedSet.add asc", mkS, (s) => { for (let i = 0; i < n; i++) s.add(k.asc[i]); }],
        ["BTree(set undef) asc", mkB, (b) => { for (let i = 0; i < n; i++) b.set(k.asc[i], undefined); }],
        ["SortedSet.add rand", mkS, (s) => { for (let i = 0; i < n; i++) s.add(k.rand[i]); }],
        ["BTree(set undef) rand", mkB, (b) => { for (let i = 0; i < n; i++) b.set(k.rand[i], undefined); }],
    ];
    for (const [label, mk, body] of rows) row(label, n, n, mk, body, trials);

    const fullS = () => { const s = new SortedSet(); for (let i = 0; i < n; i++) s.add(i); return s; };
    const fullB = () => { const b = new BTree(); for (let i = 0; i < n; i++) b.set(i, undefined); return b; };

    row("SortedSet.has", n, n, fullS, (s) => {
        let c = 0;
        for (let i = 0; i < n; i++) if (s.has(k.hit[i])) c++;
        if (c === -1) print("no");
    }, trials);
    row("BTree(set undef).has", n, n, fullB, (b) => {
        let c = 0;
        for (let i = 0; i < n; i++) if (b.has(k.hit[i])) c++;
        if (c === -1) print("no");
    }, trials);
    row("SortedSet.floor", n, n, fullS, (s) => {
        let c = 0;
        for (let i = 0; i < n; i++) if (s.floor(k.miss[i]) !== undefined) c++;
        if (c === -1) print("no");
    }, trials);
    row("BTree(set undef).floorKey", n, n, fullB, (b) => {
        let c = 0;
        for (let i = 0; i < n; i++) if (b.floorKey(k.miss[i]) !== undefined) c++;
        if (c === -1) print("no");
    }, trials);
    row("SortedSet.toArray", n, 1, fullS, (s) => {
        if (s.toArray().length !== n) print("BAD toArray");
    }, trials);
    row("BTree(set undef).keys()", n, 1, fullB, (b) => {
        if (b.keys().length !== n) print("BAD keys");
    }, trials);
    row("SortedSet.delete rand", n, n, fullS, (s) => {
        for (let i = 0; i < n; i++) s.delete(k.del[i]);
    }, trials);
    row("BTree(set undef).delete", n, n, fullB, (b) => {
        for (let i = 0; i < n; i++) b.delete(k.del[i]);
    }, trials);
}

/* ---- memory mode: one structure, one size, own process -------------------
 * Run with --std so "std" (gc) resolves. */
async function memMode(cls, n) {
    const C = { SortedMap, SortedSet, BTree }[cls];
    if (!C) { print("unknown class " + cls); return; }
    let gc = () => {};
    try { ({ gc } = await import("std")); } catch (e) {}
    gc();
    const nat0 = sys.memoryUsage().nativeSize;
    const rss0 = sys.memoryUsage().peakRss;
    const m = new C();
    for (let i = 0; i < n; i++) {
        if (cls === "SortedSet") m.add(i);
        else m.set(i, i);
    }
    gc();
    const mu = sys.memoryUsage();
    const nat = mu.nativeSize - nat0;
    const rss = mu.peakRss - rss0;
    print(cls + " n=" + n +
          " nativeDelta=" + nat + " (" + (nat / n).toFixed(1) + " B/entry)" +
          " peakRssDelta=" + rss + " (" + (rss / n).toFixed(1) + " B/entry)");
}

/* ---- driver --------------------------------------------------------------- */
const args = (typeof scriptArgs !== "undefined") ? scriptArgs : [];
if (args[1] === "--mem") {
    memMode(args[2], parseInt(args[3], 10));
} else {
    const sizes = args[1] ? args[1].split(",").map(Number) : [1e3, 1e4, 1e5, 1e6];
    print("== map family: SortedMap (skiplist) vs BTree (B+tree), ns/op ==");
    for (const n of sizes) {
        print("-- n=" + n);
        benchMap("SortedMap", n);
        benchMap("BTree", n);    }
    print("== set family: SortedSet vs BTree-as-set ==");
    for (const n of sizes) {
        print("-- n=" + n);
        benchSet(n);
    }
    for (const d of DATA) print(d);
}
