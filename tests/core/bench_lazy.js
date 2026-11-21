const N = 1e6;

function timeBest(fn, trials, minMs) {
    const floor = minMs === undefined ? 5 : minMs;
    let reps = 1;
    for (;;) {
        const t0 = performance.now();
        for (let i = 0; i < reps; i++) {
            if (fn() === undefined) throw new Error("benchmark body optimised away");
        }
        const dt = performance.now() - t0;
        if (dt >= floor || reps >= (1 << 24)) break;
        reps = Math.max(reps * 2, Math.ceil(reps * floor / Math.max(dt, 1e-4)));
    }
    let best = Infinity;
    for (let t = 0; t < trials; t++) {
        const t0 = performance.now();
        for (let i = 0; i < reps; i++) fn();
        const dt = (performance.now() - t0) / reps;
        if (dt < best) best = dt;
    }
    return best;
}

function row(name, k, eager, lazy) {
    const ratio = lazy / eager;
    print(`${name.padEnd(28)} k=${String(k).padStart(8)}  eager ${eager.toFixed(3).padStart(9)} ms` +
          `  lazy ${lazy.toFixed(3).padStart(9)} ms  ratio ${ratio.toFixed(4)}` +
          (ratio < 1 ? "  LAZY WINS" : ""));
    print(`#DATA\t${name}\t${k}\t${eager.toFixed(4)}\t${lazy.toFixed(4)}\t${ratio.toFixed(4)}`);
    return ratio;
}

const data = new Array(N);
for (let i = 0; i < N; i++) data[i] = i;

const isMul7 = (x) => (x % 7) === 0;
const times2 = (x) => x * 2;

print("=== (a) the bypass fires: filter -> take(k) over " + N + " ===");
{
    const ratios = [];
    for (const k of [1, 10, 100, 1000, 10000, 100000]) {
        const eager = timeBest(() => data.filter(isMul7).slice(0, k).length, 3);
        const lazy = timeBest(() => data.lazy().filter(isMul7).take(k).toArray().length, 3);
        ratios.push([k, row("filter+take", k, eager, lazy)]);
    }
    let cross = null;
    for (const [k, r] of ratios) if (r >= 1 && cross === null) cross = k;
    print(">>> lazy filter+take stops winning at k=" +
          (cross === null ? ">100000 (wins throughout)" : cross));
    print("");
}

print("=== (a2) the same shape with takeWhile, which the eager form cannot do lazily ===");
{
    const k = 1000;
    const eager = timeBest(() => data.map(times2).takeWhile((x) => x < 2 * k).length, 3);
    const lazy = timeBest(() => data.lazy().map(times2).takeWhile((x) => x < 2 * k)
                                   .toArray().length, 3);
    row("map+takeWhile", k, eager, lazy);
    print("");
}

print("=== (b) THE ADVERSARIAL CASE: full traversal, no early exit ===");
{
    const eager = timeBest(() => data.map(times2).length, 3);
    const lazy = timeBest(() => data.lazy().map(times2).toArray().length, 3);
    const r1 = row("full map (no exit)", N, eager, lazy);

    const eagerSum = timeBest(() => data.reduce((a, b) => a + b, 0), 3);
    const lazySum = timeBest(() => data.lazy().sum(), 3);
    const r2 = row("full sum (no exit)", N, eagerSum, lazySum);

    let sink = 0;
    const eagerFE = timeBest(() => { sink = 0; data.forEach((x) => { sink += x; }); return sink; }, 3);
    const lazyFE = timeBest(() => { sink = 0; data.lazy().forEach((x) => { sink += x; }); return sink; }, 3);
    row("full forEach (no exit)", N, eagerFE, lazyFE);

    const eagerU = timeBest(() => data.unique().length, 3);
    const lazyU = timeBest(() => data.lazy().unique().toArray().length, 3);
    row("full unique (no exit)", N, eagerU, lazyU);

    print("");
    print(">>> adversarial map ratio " + r1.toFixed(2) + "x, sum ratio " + r2.toFixed(2) +
          "x  (the tier's admission threshold is ~1.5x)");
}
