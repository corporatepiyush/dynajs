/* bench_ml_xgb.js -- the second-order objective against the first-order one
 * (STDLIB_OOP_PLAN W9.8b).
 *
 * Two questions, and they are different:
 *
 *   1. Does it cost more per round? It should cost LESS, because it skips the
 *      separate Newton line-search pass over every row -- the leaf value is
 *      already the Newton step -- and because it always bins.
 *   2. Does it fit better for the same budget? That is the claim the algorithm
 *      exists for, and a benchmark that only reported milliseconds would not
 *      test it at all.
 *
 * The adversarial row is a small fit, where binning's fixed cost is the whole
 * cost and there is nothing to amortise. It stays here permanently.
 *
 * `#B <case> <gb_ms> <xgb_ms> <ratio>` and `#Q <case> <gb_score> <xgb_score>`.
 */
import {
    GradientBoostingRegressor, GradientBoostingClassifier,
    XGBRegressor, XGBClassifier, r2Score, accuracy,
} from "dyna:ml";

function lcg(seed) { let s = seed >>> 0; return () => (s = (s * 1664525 + 1013904223) >>> 0) / 4294967296; }

function data(rows, cols, classes, seed) {
    const rnd = lcg(seed), X = new Float64Array(rows * cols), y = new Float64Array(rows);
    for (let i = 0; i < rows; i++) {
        let s = 0;
        for (let j = 0; j < cols; j++) {
            const v = rnd() * 4 - 2;
            X[i * cols + j] = v;
            s += (j + 1) * v / cols;
        }
        y[i] = classes ? (1 / (1 + Math.exp(-4 * s)) > rnd() ? 1 : 0) : s + rnd() * 0.2;
    }
    return { X, y, rows, cols };
}
/* The row form, for scoring: predict takes either, and the flat form needs the
 * dimensions passed through. */
function rowsOf(d) {
    const out = [];
    for (let i = 0; i < d.rows; i++) {
        const r = [];
        for (let j = 0; j < d.cols; j++) r.push(d.X[i * d.cols + j]);
        out.push(r);
    }
    return out;
}

function time(make, d, reps) {
    let best = Infinity;
    for (let run = 0; run < 3; run++) {
        const t0 = performance.now();
        for (let r = 0; r < reps; r++) make().fit(d.X, d.y, d.rows, d.cols);
        const dt = performance.now() - t0;
        if (dt < best) best = dt;
    }
    return best;
}

function row(name, mkGb, mkXgb, d, reps) {
    const a = time(mkGb, d, reps), b = time(mkXgb, d, reps);
    print("#B " + name + " " + a.toFixed(3) + " " + b.toFixed(3) + " " + (b / a).toFixed(3));
}

function quality(name, mkGb, mkXgb, d, classes) {
    const R = rowsOf(d), yv = Array.from(d.y);
    const g = mkGb().fit(d.X, d.y, d.rows, d.cols);
    const x = mkXgb().fit(d.X, d.y, d.rows, d.cols);
    const sg = classes ? accuracy(yv, g.predict(R)) : r2Score(yv, g.predict(R));
    const sx = classes ? accuracy(yv, x.predict(R)) : r2Score(yv, x.predict(R));
    print("#Q " + name + " " + sg.toFixed(6) + " " + sx.toFixed(6));
}

print("# GradientBoosting* vs XGB*, same rounds and depth (ratio < 1 = XGB faster)");

const big = data(2000, 20, 0, 21);
row("reg_2000x20_r50_d5",
    () => new GradientBoostingRegressor({ nEstimators: 50, maxDepth: 5 }),
    () => new XGBRegressor({ nEstimators: 50, maxDepth: 5, learningRate: 0.1 }),
    big, 1);
quality("reg_2000x20_r50_d5",
    () => new GradientBoostingRegressor({ nEstimators: 50, maxDepth: 5 }),
    () => new XGBRegressor({ nEstimators: 50, maxDepth: 5, learningRate: 0.1 }),
    big, 0);

const bigc = data(2000, 20, 2, 22);
row("clf_2000x20_r50_d5",
    () => new GradientBoostingClassifier({ nEstimators: 50, maxDepth: 5 }),
    () => new XGBClassifier({ nEstimators: 50, maxDepth: 5, learningRate: 0.1 }),
    bigc, 1);
quality("clf_2000x20_r50_d5",
    () => new GradientBoostingClassifier({ nEstimators: 50, maxDepth: 5 }),
    () => new XGBClassifier({ nEstimators: 50, maxDepth: 5, learningRate: 0.1 }),
    bigc, 1);

const mc = data(1500, 12, 3, 23);
row("multi_1500x12_r30_d4",
    () => new GradientBoostingClassifier({ nEstimators: 30, maxDepth: 4 }),
    () => new XGBClassifier({ nEstimators: 30, maxDepth: 4, learningRate: 0.1 }),
    mc, 1);

/* ---- the adversarial half ---- */
const tiny = data(120, 4, 0, 24);
row("reg_120x4_r10_d2",
    () => new GradientBoostingRegressor({ nEstimators: 10, maxDepth: 2 }),
    () => new XGBRegressor({ nEstimators: 10, maxDepth: 2, learningRate: 0.1 }),
    tiny, 60);

/* Early stopping is a cost when it never fires and a saving when it does; both
 * are reported against the same 400-round budget. */
{
    const d = data(1500, 10, 0, 25);
    const t0 = performance.now();
    const full = new XGBRegressor({ nEstimators: 400, maxDepth: 4 }).fit(d.X, d.y, d.rows, d.cols);
    const t1 = performance.now();
    const es = new XGBRegressor({ nEstimators: 400, maxDepth: 4,
                                  earlyStoppingRounds: 10, validationFraction: 0.2 })
        .fit(d.X, d.y, d.rows, d.cols);
    const t2 = performance.now();
    print("#B early_stop_400_rounds " + (t1 - t0).toFixed(3) + " " + (t2 - t1).toFixed(3) +
          " " + ((t2 - t1) / (t1 - t0)).toFixed(3));
    print("#E rounds_kept 400 " + es.bestRounds);
}
