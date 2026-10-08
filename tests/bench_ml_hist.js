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

row("stump_200x5_depth1", o => new DecisionTreeClassifier({ maxDepth: 1, ...o }),
    makeData(200, 5, 2, 15), 200, 64);

row("tiny_50x3_depth3", o => new DecisionTreeClassifier({ maxDepth: 3, ...o }),
    makeData(50, 3, 2, 16), 400, 64);

row("forest_clf_2000x20_b255", o => new RandomForestClassifier(
        { nEstimators: 10, maxDepth: 12, seed: 1, ...o }),
    makeData(2000, 20, 2, 11), 1, 255);
row("forest_clf_2000x20_b8", o => new RandomForestClassifier(
        { nEstimators: 10, maxDepth: 12, seed: 1, ...o }),
    makeData(2000, 20, 2, 11), 1, 8);
