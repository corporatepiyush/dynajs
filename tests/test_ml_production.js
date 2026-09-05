/* test_ml_production.js -- W9.4 (missing data) and W9.5 (logistic regression).
 *
 * Two production gaps, and the first one is the more dangerous because it was
 * SILENT: a NaN feature poisoned a mean, a variance, a centroid, a split
 * threshold and every coefficient downstream, and the model still "fitted".
 * The failure surfaced as NaN predictions, or -- worse -- as predictions that
 * were merely wrong, because one class's statistics were NaN and lost every
 * comparison they entered.
 *
 * Run: dynajs (CONFIG_NATIVE_MODULES=y) tests/test_ml_production.js */

import * as ml from "dyna:ml";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}
function eq(got, want, msg) {
    n++;
    if (got !== want)
        throw new Error("assertion failed: " + msg + "\n  got:  " + got +
                        "\n  want: " + want);
}
function near(got, want, tol, msg) {
    n++;
    if (!(Math.abs(got - want) <= tol))
        throw new Error("assertion failed: " + msg + "\n  got:  " + got +
                        "\n  want: " + want);
}
function throws(fn, msg) {
    n++;
    try { fn(); } catch (e) { return e; }
    throw new Error("assertion failed: " + msg + " did not throw");
}

function rng(seed) {
    let s = seed >>> 0;
    return () => { s = (Math.imul(s, 1103515245) + 12345) >>> 0; return s / 4294967296; };
}
function blobs(rows, k, seed, spread) {
    const r = rng(seed), X = [], y = [];
    for (let i = 0; i < rows; i++) {
        const c = i % k;
        X.push([c * spread + r() * 2, c * spread + r() * 2, r()]);
        y.push(c);
    }
    return [X, y];
}

/* ================================================== 1. missing data (W9.4) */

/* EVERY fitted class rejects it, from one shared check -- so a nineteenth
 * model cannot forget. The list is the point: this is not a per-model opt-in. */
{
    const [X, y] = blobs(40, 2, 3, 3);
    const bad = X.map((r) => r.slice());
    bad[7][1] = NaN;
    const supervised = [
        ["LinearRegression", () => new ml.LinearRegression()],
        ["LogisticRegression", () => new ml.LogisticRegression()],
        ["DecisionTreeClassifier", () => new ml.DecisionTreeClassifier()],
        ["DecisionTreeRegressor", () => new ml.DecisionTreeRegressor()],
        ["RandomForestClassifier", () => new ml.RandomForestClassifier({ nEstimators: 2 })],
        ["RandomForestRegressor", () => new ml.RandomForestRegressor({ nEstimators: 2 })],
        ["GradientBoostingRegressor", () => new ml.GradientBoostingRegressor({ nEstimators: 2 })],
        ["GradientBoostingClassifier", () => new ml.GradientBoostingClassifier({ nEstimators: 2 })],
        ["GaussianNB", () => new ml.GaussianNB()],
        ["KNClassifier", () => new ml.KNClassifier(3)],
        ["KNRegressor", () => new ml.KNRegressor(3)],
        ["SVC", () => new ml.SVC({ maxIter: 20 })],
    ];
    for (const [name, make] of supervised) {
        const e = throws(() => make().fit(bad, y), name + " rejects NaN in X");
        assert(/X\[7\]\[1\]/.test(e.message),
               name + " names the exact cell, not just 'NaN in input': " + e.message);
    }
    const unsupervised = [
        ["KMeans", () => new ml.KMeans(2)],
        ["PCA", () => new ml.PCA(1)],
        ["DBScan", () => new ml.DBScan(1, 2)],
        ["StandardScaler", () => new ml.StandardScaler()],
        ["MinMaxScaler", () => new ml.MinMaxScaler()],
        ["GaussianMixture", () => new ml.GaussianMixture(2, { maxIter: 5 })],
    ];
    for (const [name, make] of unsupervised)
        throws(() => make().fit(bad), name + " rejects NaN in X");

    /* Infinity is missing too: it breaks exactly the arithmetic NaN breaks. */
    const inf = X.map((r) => r.slice());
    inf[3][0] = Infinity;
    const e = throws(() => new ml.LinearRegression().fit(inf, y), "rejects Infinity");
    assert(/infinite/.test(e.message), "and says 'infinite', not 'NaN': " + e.message);

    /* a NaN TARGET cannot be imputed, and the message says so */
    const ybad = y.slice();
    ybad[5] = NaN;
    const ey = throws(() => new ml.LinearRegression().fit(X, ybad), "rejects NaN in y");
    assert(/y\[5\]/.test(ey.message) && /cannot be imputed/.test(ey.message),
           "the y message points at dropping the row: " + ey.message);

    /* clean data is unaffected -- the check must not have a false positive */
    const m = new ml.LinearRegression();
    m.fit(X, y);
    assert(m.predict(X).length === X.length, "clean data still fits");
}

/* imputeMean: column means over the finite values */
{
    const X = [[1, 10], [NaN, 20], [3, NaN], [5, 40]];
    const out = ml.imputeMean(X);
    near(out[1][0], (1 + 3 + 5) / 3, 1e-12, "column 0 mean fills the NaN");
    near(out[2][1], (10 + 20 + 40) / 3, 1e-12, "column 1 mean fills the NaN");
    eq(out[0][0], 1, "finite entries are untouched");
    eq(out[3][1], 40, "and so are the last ones");
    /* the input is not mutated -- imputation returns new data */
    assert(Number.isNaN(X[1][0]), "the caller's array is unchanged");
    /* a column with nothing finite has no mean to take */
    throws(() => ml.imputeMean([[NaN, 1], [NaN, 2]]), "an all-NaN column");
    /* and the result fits */
    const y = [1, 2, 3, 4];
    new ml.LinearRegression().fit(ml.imputeMean(X), y);
    assert(true, "imputed data is accepted by fit");
}

/* dropMissing: rows out, and the indices that survived */
{
    const X = [[1, 2], [3, NaN], [5, 6], [NaN, 8], [9, 10]];
    const y = [1, 2, 3, 4, 5];
    const d = ml.dropMissing(X, y);
    eq(JSON.stringify(d.X), JSON.stringify([[1, 2], [5, 6], [9, 10]]), "rows dropped");
    eq(JSON.stringify(Array.from(d.y)), JSON.stringify([1, 3, 5]), "y follows X");
    eq(JSON.stringify(d.kept), JSON.stringify([0, 2, 4]),
       "kept is the surviving INDICES, so other columns can follow too");
    /* a NaN in y drops the row as well */
    const d2 = ml.dropMissing([[1], [2], [3]], [1, NaN, 3]);
    eq(JSON.stringify(d2.kept), JSON.stringify([0, 2]), "a NaN target drops its row");
    /* no y at all */
    const d3 = ml.dropMissing(X);
    eq(d3.y, null, "y is null when none was given");
    eq(d3.X.length, 3, "and X is still filtered");
    /* the output is directly fittable, which is the whole point */
    new ml.LinearRegression().fit(d.X, d.y);
    assert(true, "dropMissing output is accepted by fit");
}

/* ============================================ 2. LogisticRegression (W9.5) */

/* MULTINOMIAL. It was binary-only: three classes was not expressible. */
{
    const [X, y] = blobs(300, 3, 11, 3);
    const m = new ml.LogisticRegression({ maxIter: 4000 });
    m.fit(X, y);
    eq(JSON.stringify(Array.from(m.classes)), "[0,1,2]", "classes are recorded");
    eq(m.coef.length, 3, "one weight vector per class");
    eq(m.coef[0].length, 3, "each of n_features wide");
    eq(m.intercept.length, 3, "one intercept per class");
    assert(ml.accuracy(y, m.predict(X)) > 0.95, "3-class fit works");

    const proba = m.predictProba(X);
    eq(proba[0].length, 3, "predictProba is rows x n_classes");
    for (let i = 0; i < 20; i++) {
        let s = 0, best = 0;
        for (let k = 0; k < 3; k++) {
            s += proba[i][k];
            assert(proba[i][k] >= 0 && proba[i][k] <= 1, "probability in [0,1]");
            if (proba[i][k] > proba[i][best]) best = k;
        }
        near(s, 1, 1e-12, "row " + i + " sums to 1");
        /* softmax is monotone in the raw score, so argmax cannot disagree */
        eq(m.classes[best], m.predict(X)[i], "predict === argmax(predictProba)");
    }
    /* four classes, and labels that are not 0..K-1 */
    const [X4, y4] = blobs(200, 4, 13, 3);
    const relabel = y4.map((c) => [10, 20, 30, 40][c]);
    const m4 = new ml.LogisticRegression({ maxIter: 3000 });
    m4.fit(X4, relabel);
    eq(JSON.stringify(Array.from(m4.classes)), "[10,20,30,40]",
       "arbitrary labels are preserved, not renumbered");
    assert(m4.predict(X4).every((v) => [10, 20, 30, 40].includes(v)),
           "and predictions come back in those labels");
}

/* BINARY still uses one weight vector: a second would be its exact negation. */
{
    const [X, y] = blobs(200, 2, 7, 3);
    const m = new ml.LogisticRegression();
    m.fit(X, y);
    eq(m.coef.length, 1, "two classes: one weight vector");
    eq(typeof m.intercept, "number", "and a scalar intercept");
    eq(m.predictProba(X)[0].length, 2, "but still two probability columns");
    /* labels that are not 0/1 -- the old code read `y != 0` and would have
     * called both classes 1 */
    const m2 = new ml.LogisticRegression();
    m2.fit(X, y.map((c) => c + 1));
    eq(JSON.stringify(Array.from(m2.classes)), "[1,2]", "labels 1 and 2");
    assert(ml.accuracy(y.map((c) => c + 1), m2.predict(X)) > 0.95,
           "and it separates them (reading `y != 0` could not)");
}

/* CONVERGENCE. The point of the check is that it stops early when there is
 * something to converge to -- and honestly reports when there is not. */
{
    const [Xo, yo] = blobs(200, 2, 7, 0.2);      /* heavily overlapping */
    const [Xs, ys] = blobs(200, 2, 7, 3);        /* separable */

    const conv = new ml.LogisticRegression({ l2: 0.1 });
    conv.fit(Xo, yo);
    assert(conv.converged, "a regularised fit on overlapping data converges");
    assert(conv.nIter < 3000, "and stops early (" + conv.nIter + " iterations)");

    const sep = new ml.LogisticRegression();
    sep.fit(Xs, ys);
    eq(sep.converged, false,
       "separable data with no penalty has NO finite optimum, so it runs out");
    eq(sep.nIter, 3000, "to exactly maxIter");
    assert(ml.accuracy(ys, sep.predict(Xs)) === 1,
           "which is not a failure: it still separates perfectly");

    /* maxIter is honoured */
    const capped = new ml.LogisticRegression({ maxIter: 25 });
    capped.fit(Xo, yo);
    assert(capped.nIter <= 25, "maxIter caps the run");
    /* a loose tolerance stops sooner than a tight one */
    const loose = new ml.LogisticRegression({ l2: 0.1, tol: 1e-2 });
    loose.fit(Xo, yo);
    const tight = new ml.LogisticRegression({ l2: 0.1, tol: 1e-6 });
    tight.fit(Xo, yo);
    assert(loose.nIter < tight.nIter,
           "a looser tol stops sooner (" + loose.nIter + " vs " + tight.nIter + ")");
}

/* PENALTIES. L2 shrinks the weights; that is the observable effect. */
{
    const [X, y] = blobs(200, 2, 7, 3);
    const norm = (m) => m.coef[0].reduce((a, b) => a + b * b, 0);
    const plain = new ml.LogisticRegression({ maxIter: 500 });
    plain.fit(X, y);
    const l2 = new ml.LogisticRegression({ maxIter: 500, l2: 1.0 });
    l2.fit(X, y);
    assert(norm(l2) < norm(plain),
           "L2 shrinks the weights (" + norm(l2).toFixed(3) + " < " + norm(plain).toFixed(3) + ")");
    const l1 = new ml.LogisticRegression({ maxIter: 500, l1: 1.0 });
    l1.fit(X, y);
    assert(norm(l1) < norm(plain), "L1 shrinks them too");

    /* the scikit-learn spelling: C is INVERSE strength, so a small C is a
     * strong penalty */
    const strong = new ml.LogisticRegression({ maxIter: 500, penalty: "l2", C: 0.01 });
    strong.fit(X, y);
    const weak = new ml.LogisticRegression({ maxIter: 500, penalty: "l2", C: 100 });
    weak.fit(X, y);
    assert(norm(strong) < norm(weak),
           "a smaller C penalises harder (" + norm(strong).toFixed(4) +
           " < " + norm(weak).toFixed(4) + ")");
    const none = new ml.LogisticRegression({ maxIter: 500, penalty: "none" });
    none.fit(X, y);
    near(norm(none), norm(plain), 1e-9, 'penalty:"none" equals no penalty');
    throws(() => new ml.LogisticRegression({ penalty: "quadratic" }), "an unknown penalty");
}

/* CLASS WEIGHTS on an imbalanced problem: the minority class stops being
 * ignored. Without weighting, predicting the majority everywhere scores well
 * on accuracy and 0 on recall, which is the failure mode balancing exists for. */
{
    const r = rng(31), X = [], y = [];
    for (let i = 0; i < 400; i++) {
        const minority = i % 40 === 0;            /* 2.5% positive */
        X.push([minority ? 2 + r() : r(), r(), r()]);
        y.push(minority ? 1 : 0);
    }
    const plain = new ml.LogisticRegression({ maxIter: 800 });
    plain.fit(X, y);
    const bal = new ml.LogisticRegression({ maxIter: 800, classWeight: "balanced" });
    bal.fit(X, y);
    const recall = (m) => ml.recall(y, m.predict(X));
    assert(recall(bal) >= recall(plain),
           "balancing does not reduce minority recall (" +
           recall(plain).toFixed(3) + " -> " + recall(bal).toFixed(3) + ")");
    throws(() => new ml.LogisticRegression({ classWeight: "inverse" }),
           "an unknown classWeight");
}

/* PERSISTENCE. The model's shape changed, so it needed its own codec -- it
 * used to share LinearRegression's, which cannot describe a multinomial fit. */
{
    for (const K of [2, 3, 5]) {
        const [X, y] = blobs(150, K, 17 + K, 3);
        const m = new ml.LogisticRegression({ maxIter: 400, l2: 0.05,
                                              classWeight: "balanced" });
        m.fit(X, y);
        const before = m.predictProba(X);
        const back = ml.LogisticRegression.deserialize(m.serialize());
        const after = back.predictProba(X);
        for (let i = 0; i < before.length; i++)
            for (let k = 0; k < K; k++)
                eq(after[i][k], before[i][k],
                   K + "-class row " + i + " col " + k + " is BIT-identical");
        eq(JSON.stringify(Array.from(back.classes)),
           JSON.stringify(Array.from(m.classes)), "classes survive");
        eq(back.nIter, m.nIter, "the iteration count survives");
        eq(back.converged, m.converged, "and so does whether it converged");
        /* re-encoding a loaded model reproduces the record byte for byte */
        const a = m.serialize(), b = back.serialize();
        eq(a.length, b.length, "re-encode length");
        let same = true;
        for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) same = false;
        assert(same, K + "-class record re-encodes byte-identically");
    }
}

/* the edges */
{
    const [X, y] = blobs(40, 2, 5, 3);
    throws(() => new ml.LogisticRegression().fit(X, y.map(() => 1)),
           "a single-label y has no log-odds to fit");
    throws(() => new ml.LogisticRegression().predict(X), "predict before fit");
    throws(() => new ml.LogisticRegression({ maxIter: 0 }), "maxIter 0");
    throws(() => new ml.LogisticRegression(5), "a non-object option");
    const m = new ml.LogisticRegression();
    m.fit(X, y);
    throws(() => m.predict([[1, 2]]), "the wrong feature count");
}

/* ======================================= 3. model selection (W9.7) */

/* crossValScore returns the PER-FOLD scores, not the mean, because the spread
 * is the useful part: 0.90 +- 0.01 and 0.90 +- 0.30 are different models. */
{
    const [X, y] = blobs(200, 2, 7, 1.2);
    const s = ml.crossValScore(() => new ml.DecisionTreeClassifier({ maxDepth: 4 }),
                               X, y, { folds: 4 });
    eq(s.length, 4, "one score per fold");
    for (const v of s) assert(v >= 0 && v <= 1, "accuracy in [0,1]");

    /* `folds` and `k` are the same option. An ignored option is the worst kind:
     * {folds: 4} used to run 5 folds and look like it worked. */
    eq(ml.crossValScore(() => new ml.DecisionTreeClassifier(), X, y, { k: 3 }).length, 3,
       "k: 3");
    eq(ml.crossValScore(() => new ml.DecisionTreeClassifier(), X, y, { folds: 3 }).length, 3,
       "folds: 3 means the same thing");
    eq(ml.kFold(10, { folds: 3 }).length, ml.kFold(10, { k: 3 }).length,
       "and kFold agrees with itself");

    /* a custom scorer replaces the default accuracy */
    const r2 = ml.crossValScore(() => new ml.LinearRegression(), X, y,
                                { folds: 3, scoring: (a, b) => ml.r2Score(a, b) });
    eq(r2.length, 3, "a custom scorer is used");
    for (const v of r2) assert(v <= 1, "r2 never exceeds 1");

    /* the estimator is a FACTORY: reusing one instance would score a model
     * that has already seen the test fold */
    throws(() => ml.crossValScore(new ml.DecisionTreeClassifier(), X, y),
           "an instance is rejected, a factory is required");
    throws(() => ml.crossValScore(() => ({}), X, y),
           "an object without fit/predict is rejected");
}

/* gridSearch walks the full product and reports every point */
{
    const [X, y] = blobs(200, 2, 11, 1.2);
    const g = ml.gridSearch((p) => new ml.DecisionTreeClassifier(p), X, y,
                            { maxDepth: [1, 2, 4], minSamplesLeaf: [1, 5] },
                            { folds: 3 });
    eq(g.results.length, 6, "3 x 2 = 6 points");
    assert("maxDepth" in g.best && "minSamplesLeaf" in g.best,
           "best names both parameters");
    assert(g.bestScore >= Math.max(...g.results.map((r) => r.mean)) - 1e-12,
           "bestScore is the best mean");
    for (const r of g.results) {
        eq(r.scores.length, 3, "every point has a score per fold");
        near(r.mean, Array.from(r.scores).reduce((a, b) => a + b, 0) / 3, 1e-12,
             "mean matches its scores");
    }
    /* a depth-1 stump must not beat a deeper tree on separable-ish data --
     * a search that always returned the first point would pass everything above */
    const shallow = g.results.find((r) => r.params.maxDepth === 1);
    assert(g.bestScore >= shallow.mean, "the search actually compares points");

    throws(() => ml.gridSearch((p) => new ml.DecisionTreeClassifier(p), X, y, {}),
           "an empty grid");
    throws(() => ml.gridSearch((p) => new ml.DecisionTreeClassifier(p), X, y,
                               { maxDepth: 4 }), "a non-array grid value");
    throws(() => ml.gridSearch((p) => new ml.DecisionTreeClassifier(p), X, y,
                               { maxDepth: [] }), "an empty value list");
}

/* randomSearch samples instead of enumerating, and is reproducible by seed */
{
    const [X, y] = blobs(150, 2, 13, 1.2);
    const grid = { maxDepth: [1, 2, 4, 8, 16], minSamplesLeaf: [1, 3, 5, 9] };
    const a = ml.randomSearch((p) => new ml.DecisionTreeClassifier(p), X, y, grid,
                              { folds: 3, nIter: 5, seed: 7 });
    eq(a.results.length, 5, "nIter points, not the full 20");
    const b = ml.randomSearch((p) => new ml.DecisionTreeClassifier(p), X, y, grid,
                              { folds: 3, nIter: 5, seed: 7 });
    eq(JSON.stringify(a.results.map((r) => r.params)),
       JSON.stringify(b.results.map((r) => r.params)),
       "the same seed samples the same points");
    const c = ml.randomSearch((p) => new ml.DecisionTreeClassifier(p), X, y, grid,
                              { folds: 3, nIter: 5, seed: 99 });
    assert(JSON.stringify(a.results.map((r) => r.params)) !==
           JSON.stringify(c.results.map((r) => r.params)),
           "a different seed samples different points");
    /* nIter above the grid size falls back to the whole grid */
    const full = ml.randomSearch((p) => new ml.DecisionTreeClassifier(p), X, y,
                                 { maxDepth: [1, 2] }, { folds: 2, nIter: 100 });
    eq(full.results.length, 2, "nIter is capped at the grid size");
    throws(() => ml.randomSearch((p) => new ml.DecisionTreeClassifier(p), X, y,
                                 grid, { nIter: 0 }), "nIter 0");
}
console.log("test_ml_production.js: " + n + " assertions passed");
