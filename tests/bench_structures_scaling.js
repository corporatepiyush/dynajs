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

print("BiMap");
{
    const build = (n) => { const b = new S.BiMap();
        for (let i = 0; i < n; i++) b.set("k" + i, "v" + i); return b; };
    sweep("set (new pair)", build, (b, i, n) => { b.set("z" + (n + i), "w" + (n + i)); });
    sweep("set (rebind an existing key)", build, (b, i, n) => { b.set("k" + (i % n), "w" + i); });
    sweep("get", build, (b, i, n) => b.get("k" + (i % n)) === undefined ? 0 : 1);
    sweep("keyOf (the reverse index)", build, (b, i, n) => b.keyOf("v" + (i % n)) === undefined ? 0 : 1);
    sweep("get (miss) -- CONTROL", build, (b, i, n) => b.get("absent" + i) === undefined ? 0 : 1);
    sweep("delete + set (alternating)", build, (b, i, n) => {
        b.delete("k" + (i % n)); b.set("k" + (i % n), "v" + (i % n)); });
}

print("LRU");
{
    const roomy = (n) => { const L = new S.LRU(n * 2);
        for (let i = 0; i < n; i++) L.set("k" + i, i); return L; };
    sweep("get (hit, promotes to MRU)", roomy, (L, i, n) => L.get("k" + (i % n)) === undefined ? 0 : 1);
    sweep("get (miss) -- CONTROL", roomy, (L, i, n) => L.get("absent" + i) === undefined ? 0 : 1);
    sweep("put (update in place)", roomy, (L, i, n) => { L.set("k" + (i % n), i); });
    sweep("has", roomy, (L, i, n) => L.has("k" + (i % n)) ? 1 : 0);

    const tight = (n) => { const L = new S.LRU(n);
        for (let i = 0; i < n; i++) L.set("k" + i, i); return L; };
    sweep("put (every one evicts)", tight, (L, i, n) => { L.set("new" + (n + i), i); });
    sweep("get the LRU end each time", tight, (L, i, n) => {
        const k = "k" + (i % n); return L.get(k) === undefined ? 0 : 1; });
}

print("IntervalTree");
{
    const build = (n) => { const t = new S.IntervalTree();
        for (let i = 0; i < n; i++) t.insert(i * 10, i * 10 + 25, i);
        t.at(0);
        return t; };
    sweep("at (point query)", build, (t, i, n) => t.at((i % n) * 10 + 5).length);
    sweep("overlapping (range query)", build, (t, i, n) =>
        t.overlapping((i % n) * 10, (i % n) * 10 + 50).length);
    sweep("insert (index already dirty)", build, (t, i, n) => { t.insert(i, i + 5, i); });

    sweep("insert then query (alternating)", build, (t, i, n) => {
        t.insert(1000000 + i, 1000000 + i + 5, i);
        return t.at((i % n) * 10 + 5).length;
    }, [1000, 2000, 4000, 8000], 200);

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

    const wide = (n) => { const t = new S.IntervalTree();
        for (let i = 0; i < n; i++) t.insert(0, i * 10 + 1000000, i);
        t.at(0);
        return t; };
    sweep("at (every interval matches)", wide, (t, i, n) => t.at(5).length,
          [500, 1000, 2000, 4000], 200);
}

print("");
print("sink " + (SINK > -1 ? "ok" : "?"));
