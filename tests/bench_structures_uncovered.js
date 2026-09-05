/*
 * The six dyna:structures classes the standing profile never measured:
 * Graph, IntervalTree, MinMaxHeap, RangeMap, RangeSet, Table.
 *
 * A class with no row cannot be shown to have regressed, and a gap in one is
 * invisible. Everything here follows the same rules as bench_structures_profile:
 * nothing in the timed region but the operation, the driving loop calibrated
 * away, results into a sink, and a rebuild hook where the operation GROWS the
 * structure (otherwise the size under test is not the size claimed).
 */
import * as S from "dyna:structures";

let SINK = 0;

function bench(cls, op, n, body, reps, rebuild) {
    reps = reps || Math.min(n, 100000);
    if (rebuild) rebuild();
    for (let i = 0; i < (reps / 10 | 0) + 1; i++) body(i);
    if (rebuild) rebuild();
    const t0 = performance.now();
    for (let i = 0; i < reps; i++) SINK += body(i) || 0;
    const t1 = performance.now();
    const t2 = performance.now();
    for (let i = 0; i < reps; i++) SINK += i;
    const t3 = performance.now();
    const ns = ((t1 - t0) - (t3 - t2)) * 1e6 / reps;
    print("  " + (cls + "." + op).padEnd(28) + String(n).padStart(8) +
          ns.toFixed(1).padStart(12) + " ns/op");
    return ns;
}

function record(cls, o) {
    const t0 = performance.now();
    const r = o.serialize();
    const ms = performance.now() - t0;
    print("  " + (cls + " record").padEnd(28) + String(r.length).padStart(10) +
          " bytes   (" + ms.toFixed(1) + " ms)");
    return r;
}

/* ---- MinMaxHeap: a double-ended heap, same JS-comparator question as Heap - */
for (const N of [1000, 100000]) {
    print("MinMaxHeap n=" + N);
    const I = new Array(N);
    for (let i = 0; i < N; i++) I[i] = (i * 2654435761) % 1000003;
    let h;
    const rebuild = () => { h = new S.MinMaxHeap(); for (let i = 0; i < N; i++) h.push(I[i]); };
    rebuild();
    bench("MinMaxHeap", "push", N, (i) => { h.push(I[i % N]); }, N, rebuild);
    rebuild();
    bench("MinMaxHeap", "pushPopMin", N, (i) => { h.push(I[i % N]); SINK += h.popMin(); });
    bench("MinMaxHeap", "pushPopMax", N, (i) => { h.push(I[i % N]); SINK += h.popMax(); });
    bench("MinMaxHeap", "peekMin", N, () => h.peekMin());
    if (N === 100000) record("MinMaxHeap", h);
}

/* ---- IntervalTree ------------------------------------------------------- */
for (const N of [1000, 100000]) {
    print("IntervalTree n=" + N);
    let t;
    const rebuild = () => { t = new S.IntervalTree();
        for (let i = 0; i < N; i++) t.insert(i * 10, i * 10 + 25, i); };
    rebuild();
    bench("IntervalTree", "insert", N, (i) => { t.insert(i * 10, i * 10 + 25, i); }, N, rebuild);
    rebuild();
    bench("IntervalTree", "at", N, (i) => t.at((i % N) * 10 + 5).length);
    bench("IntervalTree", "overlapping", N, (i) => t.overlapping((i % N) * 10, (i % N) * 10 + 50).length);
    if (N === 100000) record("IntervalTree", t);
}

/* ---- RangeSet / RangeMap ------------------------------------------------ */
for (const N of [1000, 100000]) {
    print("RangeSet n=" + N);
    let r;
    /* Disjoint ranges: the structure keeps them separate, which is its worst
       case for size. Adjacent ranges would coalesce into one and measure the
       coalescing instead. */
    const rebuild = () => { r = new S.RangeSet();
        for (let i = 0; i < N; i++) r.add(i * 10, i * 10 + 5); };
    rebuild();
    bench("RangeSet", "add (disjoint)", N, (i) => { r.add(i * 10, i * 10 + 5); }, N, rebuild);
    rebuild();
    bench("RangeSet", "contains", N, (i) => r.contains((i % N) * 10 + 2) ? 1 : 0);
    bench("RangeSet", "intersects", N, (i) => r.intersects((i % N) * 10, (i % N) * 10 + 3) ? 1 : 0);
    /* COALESCING is the other regime: every add merges into one run. */
    let c;
    const rebuildC = () => { c = new S.RangeSet(); };
    rebuildC();
    bench("RangeSet", "add (coalescing)", N, (i) => { c.add(i, i + 2); }, N, rebuildC);
    /* DESCENDING inserts memmove the whole tail per add: the array-backed
     * design's honest worst case, recorded so a future "fix" can be measured
     * against it. Gaps of 10 keep the ranges DISJOINT -- adjacent ones would
     * coalesce into one span and measure the coalescing instead. */
    let d;
    const rebuildD = () => { d = new S.RangeSet(); };
    rebuildD();
    bench("RangeSet", "add (descending)", N, (i) => { d.add((N - i) * 10, (N - i) * 10 + 5); }, N, rebuildD);
    /* remove() on a miss: the old linear scan walked the whole span list
     * (63.6 us at n=200k); the bisect made it O(log n). */
    bench("RangeSet", "remove (miss)", N, (i) => { r.remove(N * 10 + i, N * 10 + i + 1); });
    if (N === 100000) record("RangeSet", r);

    print("RangeMap n=" + N);
    let m;
    const rebuildM = () => { m = new S.RangeMap();
        for (let i = 0; i < N; i++) m.put(i * 10, i * 10 + 5, i); };
    rebuildM();
    bench("RangeMap", "put", N, (i) => { m.put(i * 10, i * 10 + 5, i); }, N, rebuildM);
    rebuildM();
    bench("RangeMap", "get", N, (i) => { const v = m.get((i % N) * 10 + 2); return v === undefined ? 0 : 1; });
    if (N === 100000) record("RangeMap", m);
}

/* ---- Table (row, column) -> value --------------------------------------- */
for (const N of [1000, 100000]) {
    print("Table n=" + N);
    const R = Math.max(1, Math.floor(Math.sqrt(N)));
    let t;
    const rebuild = () => { t = new S.Table();
        for (let i = 0; i < N; i++) t.put("r" + (i % R), "c" + ((i / R) | 0), i); };
    rebuild();
    bench("Table", "put", N, (i) => { t.put("r" + (i % R), "c" + ((i / R) | 0), i); }, N, rebuild);
    rebuild();
    bench("Table", "get", N, (i) => { const v = t.get("r" + (i % R), "c" + ((i / R) | 0));
                                      return v === undefined ? 0 : 1; });
    bench("Table", "row", Math.min(N, 1000), (i) => t.row("r" + (i % R)) ? 1 : 0, 20000);
    bench("Table", "column", Math.min(N, 1000), (i) => t.column("c" + (i % R)) ? 1 : 0, 20000);
    if (N === 100000) record("Table", t);
}

/* ---- Graph: build cost, and each traversal separately -------------------
   An aggregate over "graph operations" would hide which one dominates, and
   these have different complexities -- dijkstra and floydWarshall are not
   comparable to bfs and must not be averaged with it. */
{
    const N = 20000, DEG = 5;
    print("Graph n=" + N + " deg=" + DEG);
    let g;
    const rebuild = () => { g = new S.Graph();
        for (let i = 0; i < N; i++)
            for (let k = 1; k <= DEG; k++) g.addEdge(i, (i * 7 + k * 13) % N); };
    rebuild();
    bench("Graph", "addEdge", N, (i) => { g.addEdge(i % N, (i * 3) % N); }, N, rebuild);
    rebuild();
    bench("Graph", "neighbors", N, (i) => g.neighbors(i % N).length);
    bench("Graph", "hasEdge", N, (i) => g.hasEdge(i % N, (i * 7 + 13) % N) ? 1 : 0);
    /* Whole-graph algorithms: one call IS the operation, so few reps. */
    bench("Graph", "bfs", N, () => g.bfs(0).length, 20);
    bench("Graph", "dfs", N, () => g.dfs(0).length, 20);
    bench("Graph", "connectedComponents", N, () => g.connectedComponents().length, 20);
    /* topologicalSort is defined only on a DAG, so it needs its own graph. */
    {
        const dag = new S.Graph({ directed: true });
        for (let i = 0; i < N; i++)
            for (let k = 1; k <= DEG; k++) { const j = i + k * 7; if (j < N) dag.addEdge(i, j); }
        bench("Graph", "topologicalSort", N, () => { const r = dag.topologicalSort(); return r ? r.length : 0; }, 20);
    }

    const gw = new S.Graph({ weighted: true });
    for (let i = 0; i < N; i++)
        for (let k = 1; k <= DEG; k++) gw.addEdge(i, (i * 7 + k * 13) % N, 1 + (i % 9));
    bench("Graph", "dijkstra", N, () => { const r = gw.dijkstra(0); return r ? 1 : 0; }, 10);
    bench("Graph", "mst", N, () => { const r = gw.mst(); return r ? 1 : 0; }, 10);
    /* aStar pays n heuristic calls up front: the h-precompute is the cost
     * this row exists to pin. */
    bench("Graph", "aStar", N, () => { const r = gw.aStar(0, N - 1, () => 0); return r ? 1 : 0; }, 5);
    record("Graph", g);

    /* floydWarshall is O(n^3): a small graph only, but it has NO other row
     * anywhere. */
    {
        const FN = 300;
        const fg = new S.Graph({ weighted: true });
        for (let i = 0; i < FN; i++)
            for (let k = 1; k <= 4; k++) fg.addEdge(i, (i * 7 + k * 13) % FN, 1 + (i % 9));
        bench("Graph", "floydWarshall", FN, () => { const r = fg.floydWarshall(); return r ? 1 : 0; }, 3);
    }
}

print("");
print("sink " + (SINK > -1 ? "ok" : "?"));
