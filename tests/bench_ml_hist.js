/* bench_ml_hist.js -- what binning is worth, and where it is not worth anything
 * (STDLIB_OOP_PLAN W9.8a).
 *
 * The exact splitter sorts once per (node, feature): O(rows log rows) with a
 * gathered load per comparison. The histogram sorts once per (fit, feature) and
 * then reads a byte per row per node. So the win should GROW with rows and with
 * tree depth, and shrink to nothing -- or to a loss -- when the fit is small
 * enough that binning's fixed cost is the whole cost.
 *
 * Both cases are printed, per CLAUDE.md section 4: a bypass has to be measured
 * where it does not fire, not only where it does.
 *
 * The b255 row USED to be the second losing case (1.47x) and is now 0.93x. The
 * per-feature cost was O(bins) -- a full clear, a full totals pass and a full
 * sweep -- whatever the node held, so a bin count above what the data resolves
 * was pure tax. Each node now tracks the range of bins its own rows touch and
 * clears and sweeps only that, which is a BOSCC in the shape section 4
 * describes: a cheap summary (two integers, maintained in a loop that was
 * already reading the bin code) that bypasses work proportional to something
 * the node does not use. It is kept in the bench because the row is what would
 * show the tax coming back.
 *
 * Machine-readable lines are `#B <case> <exact_ms> <hist_ms> <ratio>`; the
 * ratio is hist/exact, so below 1.0 is a win.
 */
import {
    DecisionTreeClassifier, RandomForestClassifier, RandomForestRegressor,
    GradientBoostingRegressor,
} from "dyna:ml";

function lcg(seed) { let s = seed >>> 0; return () => (s = (s * 1664525 + 1013904223) >>> 0) / 4294967296; }

function makeData(rows, cols, classes, seed) {
    const rnd = lcg(seed), X = new Float64Array(rows * cols), y = new Float64Array(rows);
    for (let i = 0; i < rows; i++) {
        let s = 0;
        for (let j = 0; j < cols; j++) {
            const v = rnd() * 4 - 2;
            X[i * cols + j] = v;
            s += (j + 1) * v;
        }
        y[i] = classes ? (1 / (1 + Math.exp(-s)) > rnd() ? 1 : 0) : s + rnd() * 0.2;
    }
    return [X, y, rows, cols];
}

/* Fitting is the whole measurement, so there is nothing else in the timed
 * region -- the data is built once outside it and reused. Three runs, best
 * taken, because a fit is long enough that a scheduling hiccup is visible. */
function time(make, data, reps) {
    let best = Infinity;
    for (let run = 0; run < 3; run++) {
        const t0 = performance.now();
        for (let r = 0; r < reps; r++) make().fit(data[0], data[1], data[2], data[3]);
        const dt = performance.now() - t0;
        if (dt < best) best = dt;
    }
    return best;
}

function row(name, make, data, reps, bins) {
    const a = time(() => make({}), data, reps);
    const b = time(() => make({ maxBins: bins }), data, reps);
    print("#B " + name + " " + a.toFixed(3) + " " + b.toFixed(3) + " " +
          (b / a).toFixed(3));
    return b / a;
}

print("# fit time, exact split finding vs maxBins (ratio < 1 is a win)");

/* ---- where it fires: many rows, deep trees ---- */
row("forest_clf_2000x20_d12", o => new RandomForestClassifier(
        { nEstimators: 10, maxDepth: 12, seed: 1, ...o }),
    makeData(2000, 20, 2, 11), 1, 64);

row("forest_reg_2000x20_d12", o => new RandomForestRegressor(
        { nEstimators: 10, maxDepth: 12, seed: 1, ...o }),
    makeData(2000, 20, 0, 12), 1, 64);

row("tree_clf_5000x30_deep", o => new DecisionTreeClassifier(
        { maxDepth: 0, minSamplesLeaf: 2, ...o }),
    makeData(5000, 30, 2, 13), 1, 64);

row("boost_reg_2000x20_r50", o => new GradientBoostingRegressor(
        { nEstimators: 50, maxDepth: 5, ...o }),
    makeData(2000, 20, 0, 14), 1, 64);

/* ---- the adversarial half: the bypass cannot pay ---- *
 * A stump over a few rows does one split, so binning pays its whole per-fit
 * sort to save one per-node sort. This row is expected to LOSE and is kept
 * permanently so the loss stays visible. */
row("stump_200x5_depth1", o => new DecisionTreeClassifier({ maxDepth: 1, ...o }),
    makeData(200, 5, 2, 15), 200, 64);

row("tiny_50x3_depth3", o => new DecisionTreeClassifier({ maxDepth: 3, ...o }),
    makeData(50, 3, 2, 16), 400, 64);

/* Resolution is a cost too: 255 bins over 2000 rows means the per-node sweep
 * is 255 steps where 8 bins would be 8, while the accumulation pass is the
 * same either way. */
row("forest_clf_2000x20_b255", o => new RandomForestClassifier(
        { nEstimators: 10, maxDepth: 12, seed: 1, ...o }),
    makeData(2000, 20, 2, 11), 1, 255);
row("forest_clf_2000x20_b8", o => new RandomForestClassifier(
        { nEstimators: 10, maxDepth: 12, seed: 1, ...o }),
    makeData(2000, 20, 2, 11), 1, 8);
