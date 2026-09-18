/* bench_lazy.js -- the mandatory adversarial pair for the lazy tier (W6.5).
 *
 * A lazy pipeline is a BOSCC in the CLAUDE.md section 4 sense: the cheap
 * summary is "the consumer stopped asking", and it bypasses every element the
 * eager form would have materialised. So it must be measured in BOTH
 * directions, and the losing one has to stay in the file forever:
 *
 *   (a) BYPASS FIRES -- filter then take(10) over 1M. Eager builds a whole
 *       intermediate array; lazy touches ~the first few. Lazy should win by
 *       orders of magnitude.
 *   (b) BYPASS NEVER FIRES -- a full map over 1M with no early exit. Nothing
 *       is skipped, so lazy pays per-element helper-call overhead for nothing
 *       and MUST lose. The plan's threshold: worse than ~1.5x means the
 *       per-element cost needs work before the tier is advertised.
 *
 * Also reports the crossover: the take(k) at which the eager form catches up.
 *
 * Emits `#DATA<TAB>case<TAB>n<TAB>eager_ms<TAB>lazy_ms<TAB>ratio` where ratio
 * is lazy/eager, so below 1.0 is a lazy win.
 *
 * Run: dynajs tests/bench_lazy.js */

const N = 1e6;

/* Best of `trials`, each trial running `fn` enough times to land past the
 * ~1us resolution of performance.now(). A k=1 lazy pipeline finishes in tens of
 * nanoseconds; timing it once reports quantised garbage (CLAUDE.md section 3),
 * which is exactly the case this bench exists to show off. */
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

/* Built once, outside every timed region: building it is not what is measured,
 * and rebuilding per rep would time the allocator instead. */
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
    /* map over everything. The lazy form skips nothing, so it can only lose;
     * the number is the per-element cost of the helper machinery. */
    const eager = timeBest(() => data.map(times2).length, 3);
    const lazy = timeBest(() => data.lazy().map(times2).toArray().length, 3);
    const r1 = row("full map (no exit)", N, eager, lazy);

    /* the same, reduced to a scalar so no result array is built on either side */
    const eagerSum = timeBest(() => data.reduce((a, b) => a + b, 0), 3);
    const lazySum = timeBest(() => data.lazy().sum(), 3);
    const r2 = row("full sum (no exit)", N, eagerSum, lazySum);

    /* Isolate the per-element machinery from the result-building: forEach runs
     * the same closure per element on both sides and builds nothing. Whatever
     * the map row costs above this is the intermediate array, not the tier. */
    let sink = 0;
    const eagerFE = timeBest(() => { sink = 0; data.forEach((x) => { sink += x; }); return sink; }, 3);
    const lazyFE = timeBest(() => { sink = 0; data.lazy().forEach((x) => { sink += x; }); return sink; }, 3);
    row("full forEach (no exit)", N, eagerFE, lazyFE);

    /* one lazy-only method against its eager twin, so the tier's own methods
     * are measured and not just map */
    const eagerU = timeBest(() => data.unique().length, 3);
    const lazyU = timeBest(() => data.lazy().unique().toArray().length, 3);
    row("full unique (no exit)", N, eagerU, lazyU);

    print("");
    print(">>> adversarial map ratio " + r1.toFixed(2) + "x, sum ratio " + r2.toFixed(2) +
          "x  (the tier's admission threshold is ~1.5x)");
}
