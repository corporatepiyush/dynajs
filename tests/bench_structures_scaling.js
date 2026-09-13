/*
 * Scaling sweeps for the classes with no measured verdict: BiMap, LRU and
 * IntervalTree.
 *
 * A single timing is a constant and cannot tell you whether an operation is
 * O(1) or O(n) -- three of this module's operations turned out to be O(n) per
 * call and every one of them looked like a plausible number in isolation. So
 * every row here DOUBLES n and prints the ratio: ~1.0 is O(1) or O(log n),
 * ~2.0 is O(n) per call and therefore O(n^2) to build.
 *
 * Each family also carries a CONTROL: an operation already known to be flat.
 * If the control moves, the harness is lying and no other row means anything.
 *
 * The alternation rows exist because a structure that rebuilds an index on
 * mutation is fast when read repeatedly and quadratic when reads and writes
 * interleave -- that is the shape that made Table.row slower than the scan it
 * replaced, and it is invisible to a benchmark that only reads.
 */
import * as S from "dyna:structures";

let SINK = 0;

function per(n, build, op, reps) {
    const o = build(n);
    reps = reps || Math.min(n, 2000);
    for (let i = 0; i < (reps / 10 | 0) + 1; i++) op(o, i, n);
    const t0 = performance.now();
    for (let i = 0; i < reps; i++) SINK += op(o, i, n) || 0;
    const t1 = performance.now();
    const t2 = performance.now();
    for (let i = 0; i < reps; i++) SINK += i;
    const t3 = performance.now();
    return ((t1 - t0) - (t3 - t2)) * 1e6 / reps;
}

function sweep(label, build, op, sizes, reps) {
    let prev = 0, worst = 0;
    const rows = [];
    for (const n of (sizes || [2000, 4000, 8000, 16000, 32000])) {
        const ns = per(n, build, op, reps);
        const ratio = prev ? ns / prev : 0;
        if (ratio > worst) worst = ratio;
        rows.push("n=" + String(n).padStart(6) + " " + ns.toFixed(1).padStart(10) +
                  " ns" + (prev ? "  x" + ratio.toFixed(2) : "        "));
        prev = ns;
    }
    const verdict = worst >= 1.7 ? "  <<< O(n) PER CALL" : worst >= 1.25 ? "  (grows)" : "";
    print("  " + label.padEnd(34) + rows.join("   ") + verdict);
}

/* ---- BiMap: a two-way index, so every write touches two hash tables ------ */
print("BiMap");
{
    const build = (n) => { const b = new S.BiMap();
        for (let i = 0; i < n; i++) b.set("k" + i, "v" + i); return b; };
    sweep("set (new pair)", build, (b, i, n) => { b.set("z" + (n + i), "w" + (n + i)); });
    sweep("set (rebind an existing key)", build, (b, i, n) => { b.set("k" + (i % n), "w" + i); });
    sweep("get", build, (b, i, n) => b.get("k" + (i % n)) === undefined ? 0 : 1);
    sweep("keyOf (the reverse index)", build, (b, i, n) => b.keyOf("v" + (i % n)) === undefined ? 0 : 1);
    /* CONTROL: a miss touches one table and returns; it must stay flat. */
    sweep("get (miss) -- CONTROL", build, (b, i, n) => b.get("absent" + i) === undefined ? 0 : 1);
    /* ALTERNATION: delete and re-add, which swap-removes in the dense array. */
    sweep("delete + set (alternating)", build, (b, i, n) => {
        b.delete("k" + (i % n)); b.set("k" + (i % n), "v" + (i % n)); });
}

/* ---- LRU: a hash table plus an intrusive list ---------------------------- */
print("LRU");
{
    /* Capacity ABOVE n, so nothing is evicted and this measures lookup only. */
    const roomy = (n) => { const L = new S.LRU(n * 2);
        for (let i = 0; i < n; i++) L.set("k" + i, i); return L; };
    sweep("get (hit, promotes to MRU)", roomy, (L, i, n) => L.get("k" + (i % n)) === undefined ? 0 : 1);
    sweep("get (miss) -- CONTROL", roomy, (L, i, n) => L.get("absent" + i) === undefined ? 0 : 1);
    sweep("put (update in place)", roomy, (L, i, n) => { L.set("k" + (i % n), i); });
    sweep("has", roomy, (L, i, n) => L.has("k" + (i % n)) ? 1 : 0);

    /* EVICTION is the other regime: capacity EQUAL to n, so every new key
       evicts one. A structure that scans to find the victim shows here. */
    const tight = (n) => { const L = new S.LRU(n);
        for (let i = 0; i < n; i++) L.set("k" + i, i); return L; };
    sweep("put (every one evicts)", tight, (L, i, n) => { L.set("new" + (n + i), i); });
    /* Always fetching the OLDEST key is the worst promotion pattern: it moves
       the tail to the head every time. */
    sweep("get the LRU end each time", tight, (L, i, n) => {
        const k = "k" + (i % n); return L.get(k) === undefined ? 0 : 1; });
}

/* ---- IntervalTree: a sorted array plus a max-hi segment tree ------------- */
print("IntervalTree");
{
    const build = (n) => { const t = new S.IntervalTree();
        for (let i = 0; i < n; i++) t.insert(i * 10, i * 10 + 25, i);
        t.at(0);                       /* force the index to be built once */
        return t; };
    sweep("at (point query)", build, (t, i, n) => t.at((i % n) * 10 + 5).length);
    sweep("overlapping (range query)", build, (t, i, n) =>
        t.overlapping((i % n) * 10, (i % n) * 10 + 50).length);
    sweep("insert (index already dirty)", build, (t, i, n) => { t.insert(i, i + 5, i); });

    /* THE ALTERNATION. insert() dirties the index and the next query rebuilds
       it -- a sort of the whole array. If this row is O(n) while `at` alone is
       flat, the rebuild is the cost and it is the same shape that made
       Table.row slower than the scan it replaced. */
    sweep("insert then query (alternating)", build, (t, i, n) => {
        t.insert(1000000 + i, 1000000 + i + 5, i);
        return t.at((i % n) * 10 + 5).length;
    }, [1000, 2000, 4000, 8000], 200);

    /* THE PENDING-TAIL REGIME. A burst of inserts JUST under the rebuild
       threshold (16 + built/8) followed by queries: the first query of the
       burst scans the tail, the second (qsince) rebuilds, and the rest are
       clean. The second-call trigger is what keeps the whole burst from
       paying the scan per query -- measured 43.5 us/query vs 0.5 us clean. */
    {
        const burst = (n) => { const t = new S.IntervalTree();
            for (let i = 0; i < n; i++) t.insert(i * 10, i * 10 + 25, i);
            t.at(0);
            const pending = Math.max(16, (n / 8 | 0)) - 2;
            for (let i = 0; i < pending; i++) t.insert(1e12 + i, 1e12 + i + 5, i);
            return t; };
        sweep("query (burst 1st -- scans tail)", burst,
              (t, i, n) => t.at((i % n) * 10 + 5).length,
              [200000, 400000, 800000], 1);
        sweep("query (burst 2nd -- rebuilds)", burst,
              (t, i, n) => t.at((i % n) * 10 + 5).length,
              [200000, 400000, 800000], 11);
    }

    /* A query returning MANY intervals is a different cost from one returning
       few: the first is bounded by the result, the second by the descent. */
    const wide = (n) => { const t = new S.IntervalTree();
        for (let i = 0; i < n; i++) t.insert(0, i * 10 + 1000000, i);
        t.at(0);
        return t; };
    sweep("at (every interval matches)", wide, (t, i, n) => t.at(5).length,
          [500, 1000, 2000, 4000], 200);
}

print("");
print("sink " + (SINK > -1 ? "ok" : "?"));
