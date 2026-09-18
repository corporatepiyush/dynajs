/* bench_ml_trees.js — dyna:ml tree/forest cost baseline.
 *
 * bench_ml.js covers the linear/clustering/mixture families; trees were never
 * benchmarked, yet dyn_tree_build has the worst vector:scalar ratio in the
 * module (vec=34 scalarFP=54) and its split sweep is the hot loop for every
 * DecisionTree*, RandomForest* and GradientBoosting fit.
 *
 * Flat Float64Array form throughout, so what is measured is native math and not
 * JS-array marshalling. Setup is outside the timed region.
 * Run: ./dynajs -m tests/bench_ml_trees.js
 */
import { DecisionTreeClassifier, DecisionTreeRegressor,
         RandomForestClassifier, RandomForestRegressor,
         GradientBoostingRegressor } from "dyna:ml";

const REPS = 5;

function bench(name, setup, run) {
    const ctx = setup();
    run(ctx);                       /* warm */
    let best = Infinity;
    for (let r = 0; r < REPS; r++) {
        const t0 = performance.now();
        run(ctx);
        const dt = performance.now() - t0;
        if (dt < best) best = dt;
    }
    print(name.padEnd(44) + best.toFixed(3).padStart(10) + " ms");
    print("#DATA\t" + name + "\t" + best.toFixed(4));
    return best;
}

function lcg(seed) {
    let s = seed >>> 0;
    return () => (s = (Math.imul(s, 1103515245) + 12345) >>> 0) / 4294967296;
}

/* continuous features so the split sweep sees mostly-distinct values -- the
   case where the per-sample loop actually runs end to end */
function makeData(rows, cols, nclass, seed) {
    const rnd = lcg(seed);
    const X = new Float64Array(rows * cols);
    const y = new Float64Array(rows);
    for (let i = 0; i < rows; i++) {
        const cls = i % nclass;
        for (let j = 0; j < cols; j++)
            X[i * cols + j] = rnd() * 2 - 1 + cls * 0.6;
        y[i] = nclass > 1 ? cls : rnd() * 10;
    }
    return { X, y, rows, cols };
}

/* ties: many repeated values, exercising the xcur==xprev early-out */
function makeTied(rows, cols, nclass, seed) {
    const rnd = lcg(seed);
    const X = new Float64Array(rows * cols);
    const y = new Float64Array(rows);
    for (let i = 0; i < rows; i++) {
        const cls = i % nclass;
        for (let j = 0; j < cols; j++)
            X[i * cols + j] = Math.floor(rnd() * 5);
        y[i] = cls;
    }
    return { X, y, rows, cols };
}

const d1 = makeData(4000, 16, 3, 1);
const d2 = makeData(2000, 64, 3, 2);
const d3 = makeData(4000, 16, 1, 3);      /* regression */
const d4 = makeTied(4000, 16, 3, 4);

print("dyna:ml trees — best of " + REPS + "\n");

bench("tree.clf.fit   rows=4000 cols=16", () => d1,
      (d) => new DecisionTreeClassifier({ maxDepth: 10 }).fit(d.X, d.y, d.rows, d.cols));
bench("tree.clf.fit   rows=2000 cols=64", () => d2,
      (d) => new DecisionTreeClassifier({ maxDepth: 10 }).fit(d.X, d.y, d.rows, d.cols));
bench("tree.clf.fit   tied rows=4000 c=16", () => d4,
      (d) => new DecisionTreeClassifier({ maxDepth: 10 }).fit(d.X, d.y, d.rows, d.cols));
bench("tree.reg.fit   rows=4000 cols=16", () => d3,
      (d) => new DecisionTreeRegressor({ maxDepth: 10 }).fit(d.X, d.y, d.rows, d.cols));
bench("forest.clf.fit rows=4000 c=16 n=10", () => d1,
      (d) => new RandomForestClassifier({ nTrees: 10, maxDepth: 8 }).fit(d.X, d.y, d.rows, d.cols));
bench("forest.reg.fit rows=4000 c=16 n=10", () => d3,
      (d) => new RandomForestRegressor({ nTrees: 10, maxDepth: 8 }).fit(d.X, d.y, d.rows, d.cols));
bench("gbr.fit        rows=4000 c=16 n=20", () => d3,
      (d) => new GradientBoostingRegressor({ nTrees: 20, maxDepth: 3 }).fit(d.X, d.y, d.rows, d.cols));

const fitted = new DecisionTreeClassifier({ maxDepth: 10 }).fit(d1.X, d1.y, d1.rows, d1.cols);
bench("tree.clf.predict rows=4000 c=16", () => ({ m: fitted, d: d1 }),
      (o) => o.m.predict(o.d.X, o.d.rows, o.d.cols));
