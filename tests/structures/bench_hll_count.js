import { HyperLogLog } from "dyna:structures";

let sink = 0;

function time(reps, body) {
    for (let i = 0; i < (reps / 10 | 0) + 1; i++) body(i);
    const t0 = performance.now();
    for (let i = 0; i < reps; i++) sink += body(i);
    const t1 = performance.now();
    const t2 = performance.now();
    for (let i = 0; i < reps; i++) sink += i;
    const t3 = performance.now();
    return ((t1 - t0) - (t3 - t2)) * 1e6 / reps;
}

function row(label, ns) {
    print(label.padEnd(46) + ns.toFixed(1).padStart(11) + " ns");
}

for (const P of [14, 10]) {
    const M = 1 << P;
    print("--- precision " + P + " (" + M + " registers) ---");

    const h = new HyperLogLog(P);
    for (let i = 0; i < 100000; i++) h.add("key" + i);

    row("count() again, nothing added", time(20000, () => h.count()));

    let ctr = 0;
    const K = 200;
    const addsOnly = time(2000, () => {
        let s = 0;
        for (let k = 0; k < K; k++) s += h.add("z" + (ctr++));
        return s;
    });
    const addsPlusCount = time(2000, () => {
        let s = 0;
        for (let k = 0; k < K; k++) s += h.add("z" + (ctr++));
        return s + h.count();
    });
    row("  " + K + " adds alone (control)", addsOnly);
    row("  " + K + " adds + one count", addsPlusCount);
    row("count() recomputing (by difference)", addsPlusCount - addsOnly);
    print("");
}

{
    const h = new HyperLogLog(14);
    for (let i = 0; i < 1000; i++) h.add("a" + i);
    const before = h.count();
    for (let i = 0; i < 200000; i++) h.add("b" + i);
    const after = h.count();
    print("estimate moved " + before.toFixed(0) + " -> " + after.toFixed(0) +
          (after > before * 10 ? "  (cache invalidation is live)"
                               : "  *** FROZEN -- the numbers above are void"));
}

print("sink " + (sink > -1e308 ? "ok" : "?"));
