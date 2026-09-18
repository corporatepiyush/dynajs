/* test_ml_boosting.js -- GradientBoostingClassifier (W9.3).
 *
 * Boosting for classification is not boosting for regression with a threshold
 * on the end. Two things have to be right and neither shows up in an accuracy
 * number on separable data:
 *
 *   1. THE LEAF VALUES. The tree picks its partition by least squares on the
 *      gradient, but the value inside each region has to come from the loss
 *      actually being minimised (Friedman's one-step Newton line search). Fit
 *      squared error to the residual instead and the model converges to the
 *      wrong place, slowly. This file computes the expected leaf value in JS
 *      from the public API alone -- `apply()` says which rows share a leaf --
 *      and checks the model against it to 1e-9.
 *   2. THE LINK. predictProba is sigmoid/softmax of the raw score, not an
 *      average of leaf distributions (a boosted tree has none: it fits
 *      gradients). Which makes predict == argmax(predictProba) true BY
 *      CONSTRUCTION here, unlike the forest -- and that is asserted, because
 *      it is the defect W9.2 found in the forest.
 *
 * Run: dynajs (CONFIG_NATIVE_MODULES=y) tests/test_ml_boosting.js */

import {
    GradientBoostingClassifier, GradientBoostingRegressor,
    DecisionTreeClassifier, accuracy, logLoss, rocAuc,
} from "dyna:ml";

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
                        "\n  want: " + want + " (tol " + tol + ")");
}
function throws(fn, msg) {
    n++;
    try { fn(); } catch (e) { return; }
    throw new Error("assertion failed: " + msg + " did not throw");
}

/* A deterministic LCG, so every number in this file is reproducible. */
function rng(seed) {
    let s = seed >>> 0;
    return () => { s = (Math.imul(s, 1103515245) + 12345) >>> 0; return s / 4294967296; };
}

/* k separable blobs plus a pure-noise third feature, so featureImportances has
 * something it should ignore. */
function blobs(rows, k, seed, spread) {
    const r = rng(seed), X = [], y = [];
    for (let i = 0; i < rows; i++) {
        const c = i % k;
        X.push([c * 2 + r() * spread, c * 2 + r() * spread, r()]);
        y.push(c);
    }
    return [X, y];
}

/* A deliberately hard binary problem: one informative feature, heavy label
 * noise. A single tree overfits it; boosting should not. */
function noisy(rows, seed) {
    const r = rng(seed), X = [], y = [];
    for (let i = 0; i < rows; i++) {
        const x = r() * 4 - 2;
        const p = 1 / (1 + Math.exp(-1.5 * x));
        X.push([x, r(), r()]);
        y.push(r() < p ? 1 : 0);
    }
    return [X, y];
}

/* ------------------------------------------------------- 1. the leaf values
 * One round, one split, computed by hand from the public API.
 *
 *   base   = log(pos / neg)                 (the log-odds of the prior)
 *   p_i    = sigmoid(base), the same for every row before any tree
 *   grad_i = y_i - p_i,  hess_i = p_i (1 - p_i)
 *   gamma_j = sum_{i in leaf j} grad_i / sum_{i in leaf j} hess_i
 *   F_i    = base + lr * gamma_{leaf(i)}
 *
 * If the implementation used the tree's own leaf mean (the regression value)
 * instead of gamma, every one of these would be off by the factor
 * 1 / mean(hess), which for a balanced problem is 4. */
{
    const [X, y] = blobs(60, 2, 3, 1.0);
    const lr = 0.3;
    const m = new GradientBoostingClassifier({ nEstimators: 1, maxDepth: 1,
                                               learningRate: lr });
    m.fit(X, y);
    const pos = y.reduce((a, b) => a + b, 0), neg = y.length - pos;
    const base = Math.log(pos / neg);
    const p0 = 1 / (1 + Math.exp(-base));
    const leaves = m.apply(X);                 /* rows x 1 for a one-round model */
    eq(leaves.length, X.length, "apply returns one row per sample");
    eq(leaves[0].length, 1, "one output x one round = one tree");

    const num = new Map(), den = new Map();
    for (let i = 0; i < X.length; i++) {
        const leaf = leaves[i][0];
        num.set(leaf, (num.get(leaf) || 0) + (y[i] - p0));
        den.set(leaf, (den.get(leaf) || 0) + p0 * (1 - p0));
    }
    assert(num.size >= 2, "the stump actually split (found " + num.size + " leaves)");

    const proba = m.predictProba(X);
    for (let i = 0; i < X.length; i++) {
        const leaf = leaves[i][0];
        const gamma = den.get(leaf) > 1e-12 ? num.get(leaf) / den.get(leaf) : 0;
        const F = base + lr * gamma;
        near(proba[i][1], 1 / (1 + Math.exp(-F)), 1e-9,
             "row " + i + " probability is sigmoid(base + lr*gamma)");
        break;   /* one exact check per leaf is enough; the loop below does all */
    }
    let checked = 0;
    for (const leaf of num.keys()) {
        const i = leaves.findIndex((l) => l[0] === leaf);
        const gamma = den.get(leaf) > 1e-12 ? num.get(leaf) / den.get(leaf) : 0;
        near(proba[i][1], 1 / (1 + Math.exp(-(base + lr * gamma))), 1e-9,
             "leaf " + leaf + " Newton value");
        checked++;
    }
    assert(checked === num.size, "every leaf checked");
}

/* --------------------------------------------------- 2. the link, and argmax
 * predict must equal argmax(predictProba). For the forest these are different
 * rules and disagree on real data (see the W9.2 section of the plan); here
 * argmax of a monotone link of the raw score is the same argmax, and a
 * regression that broke that would be silent. */
for (const K of [2, 3, 4]) {
    const [X, y] = blobs(160, K, 17 + K, 1.4);
    const m = new GradientBoostingClassifier({ nEstimators: 20, maxDepth: 2,
                                               learningRate: 0.3 });
    m.fit(X, y);
    const pred = m.predict(X), proba = m.predictProba(X);
    eq(proba.length, X.length, K + "-class proba row count");
    eq(proba[0].length, K, K + "-class proba column count");
    let disagree = 0;
    for (let i = 0; i < X.length; i++) {
        let s = 0, best = 0;
        for (let c = 0; c < K; c++) {
            s += proba[i][c];
            assert(proba[i][c] >= 0 && proba[i][c] <= 1,
                   "probability in [0,1] (" + proba[i][c] + ")");
            if (proba[i][c] > proba[i][best]) best = c;
        }
        near(s, 1, 1e-12, "row " + i + " probabilities sum to 1");
        if (best !== pred[i]) disagree++;
    }
    eq(disagree, 0, K + "-class predict === argmax(predictProba)");
    assert(accuracy(y, pred) > 0.95, K + "-class separable accuracy (" +
           accuracy(y, pred) + ")");
}

/* ------------------------------------------------------- 3. it actually boosts
 * More rounds must not increase the training loss, and the ensemble must beat
 * a single tree of the same depth on a noisy problem. Accuracy on separable
 * data would pass with the leaf values wrong; this would not. */
{
    const [X, y] = noisy(400, 91);
    const losses = [];
    for (const rounds of [1, 5, 20, 60]) {
        const m = new GradientBoostingClassifier({ nEstimators: rounds,
                                                   maxDepth: 2,
                                                   learningRate: 0.15 });
        m.fit(X, y);
        losses.push(logLoss(y, m.predictProba(X).map((r) => r[1])));
    }
    for (let i = 1; i < losses.length; i++)
        assert(losses[i] <= losses[i - 1] + 1e-9,
               "training loss is monotone in rounds: " + losses.join(" -> "));
    assert(losses[3] < losses[0] * 0.95,
           "60 rounds beats 1 round by more than noise (" + losses.join(" -> ") + ")");

    const stump = new DecisionTreeClassifier({ maxDepth: 2 });
    stump.fit(X, y);
    const gb = new GradientBoostingClassifier({ nEstimators: 60, maxDepth: 2,
                                                learningRate: 0.15 });
    gb.fit(X, y);
    assert(rocAuc(y, gb.predictProba(X).map((r) => r[1])) >=
           rocAuc(y, stump.predictProba(X).map((r) => r[1])),
           "boosting is at least as good as one tree of the same depth");
}

/* learningRate shrinks the step: a smaller rate must move probabilities less
 * far from the prior after the same number of rounds */
{
    const [X, y] = blobs(120, 2, 5, 1.2);
    const pos = y.reduce((a, b) => a + b, 0);
    const prior = pos / y.length;
    const dist = (lr) => {
        const m = new GradientBoostingClassifier({ nEstimators: 5, maxDepth: 2,
                                                   learningRate: lr });
        m.fit(X, y);
        return m.predictProba(X).reduce((a, r) => a + Math.abs(r[1] - prior), 0);
    };
    assert(dist(0.05) < dist(0.5), "a smaller learningRate moves less");
}

/* subsample takes a different path through the round loop (a shuffled prefix,
 * one draw per round shared by every output) and must still fit */
{
    const [X, y] = blobs(200, 3, 23, 1.3);
    const m = new GradientBoostingClassifier({ nEstimators: 15, maxDepth: 2,
                                               subsample: 0.6, seed: 4 });
    m.fit(X, y);
    assert(accuracy(y, m.predict(X)) > 0.9, "subsampled fit still learns");
    const m2 = new GradientBoostingClassifier({ nEstimators: 15, maxDepth: 2,
                                                subsample: 0.6, seed: 4 });
    m2.fit(X, y);
    eq(JSON.stringify(m2.predict(X)), JSON.stringify(m.predict(X)),
       "the same seed reproduces the same subsampled fit");
}

/* ------------------------------------------------------------- 4. refitting
 * A classifier's tree count is rounds x outputs, and outputs depends on the
 * number of LABELS -- so refitting with a different class count changes the
 * size of the tree array. Freeing the new array by the old count is a heap
 * overflow, and this is the case that would trip it. */
{
    const m = new GradientBoostingClassifier({ nEstimators: 8, maxDepth: 2 });
    const [X2, y2] = blobs(90, 2, 31, 1.0);
    const [X4, y4] = blobs(120, 4, 32, 1.2);
    m.fit(X2, y2);
    eq(m.apply(X2)[0].length, 8, "two classes: one tree per round");
    m.fit(X4, y4);
    eq(m.apply(X4)[0].length, 32, "four classes: four trees per round");
    eq(m.predictProba(X4)[0].length, 4, "refit widened the probability rows");
    m.fit(X2, y2);
    eq(m.apply(X2)[0].length, 8, "and narrows again");
    assert(accuracy(y2, m.predict(X2)) > 0.95, "the narrowed refit still works");
}

/* --------------------------------------------------------- 5. persistence */
{
    const [X, y] = blobs(150, 3, 41, 1.3);
    const m = new GradientBoostingClassifier({ nEstimators: 12, maxDepth: 2,
                                               learningRate: 0.25 });
    m.fit(X, y);
    const before = m.predictProba(X);
    const bytes = m.serialize();
    const back = GradientBoostingClassifier.deserialize(bytes);
    const after = back.predictProba(X);
    eq(after.length, before.length, "loaded model predicts the same shape");
    for (let i = 0; i < before.length; i++)
        for (let c = 0; c < before[i].length; c++)
            eq(after[i][c], before[i][c],
               "row " + i + " col " + c + " is BIT-identical after a round trip");
    const again = back.serialize();
    eq(again.length, bytes.length, "re-encoding a loaded model is the same length");
    let same = true;
    for (let i = 0; i < bytes.length; i++) if (again[i] !== bytes[i]) same = false;
    assert(same, "re-encoding a loaded model is byte-identical");
}

/* A forged record must be rejected, not loaded into a model that strides its
 * own tree array by a number the file chose. n_out is exactly such a number. */
{
    const [X, y] = blobs(60, 3, 43, 1.2);
    const m = new GradientBoostingClassifier({ nEstimators: 4, maxDepth: 2 });
    m.fit(X, y);
    const good = m.serialize();
    let rejected = 0, loaded = 0;
    /* Walk the payload flipping one byte at a time in the header region where
     * n_out, n_trees and n_classes live. Every outcome must be either a throw
     * or a model that still answers -- never a crash. */
    for (let off = 12; off < 120 && off < good.length; off++) {
        for (const bit of [0x01, 0x80]) {
            const bad = new Uint8Array(good);
            bad[off] ^= bit;
            try {
                const mm = GradientBoostingClassifier.deserialize(bad);
                mm.predict(X);
                loaded++;
            } catch (e) { rejected++; }
        }
    }
    assert(rejected > 0, "forged records are rejected (" + rejected +
           " rejected, " + loaded + " loaded and survived prediction)");
}

/* ------------------------------------------------------------- 6. the edges */
throws(() => new GradientBoostingClassifier({ nEstimators: 5 }).predict([[1, 2, 3]]),
       "predict before fit");
throws(() => new GradientBoostingClassifier({ nEstimators: 5 }).predictProba([[1]]),
       "predictProba before fit");
throws(() => GradientBoostingClassifier({ nEstimators: 5 }),
       "calling the constructor without new");
{
    const [X, y] = blobs(40, 2, 51, 1.0);
    const m = new GradientBoostingClassifier({ nEstimators: 4, maxDepth: 2 });
    m.fit(X, y);
    throws(() => m.predict([[1, 2]]), "a row with the wrong feature count");
    /* One label is not a classification problem for a booster: there is no
     * log-odds to fit. A DecisionTreeClassifier accepts it (it just memorises
     * the label), so this divergence is deliberate and worth pinning -- the
     * alternative was a degenerate branch reading past a one-element class
     * table, which is what the first version of this test found. */
    throws(() => new GradientBoostingClassifier({ nEstimators: 3, maxDepth: 1 })
                     .fit([[0], [1], [2], [3]], [7, 7, 7, 7]),
           "fitting a single-label y");
    const tree = new DecisionTreeClassifier({ maxDepth: 1 });
    tree.fit([[0], [1], [2], [3]], [7, 7, 7, 7]);
    eq(tree.predict([[2]])[0], 7, "a tree still accepts a single-label y");
}

/* the regressor is unchanged by the shared refactor: its base is now a
 * one-element array, and its predictions must not have moved */
{
    const r = rng(77), X = [], y = [];
    for (let i = 0; i < 100; i++) { const a = r() * 3, b = r(); X.push([a, b]); y.push(2 * a + b); }
    const g = new GradientBoostingRegressor({ nEstimators: 40, maxDepth: 3,
                                              learningRate: 0.1 });
    g.fit(X, y);
    const pred = g.predict(X);
    let sse = 0;
    for (let i = 0; i < y.length; i++) sse += (pred[i] - y[i]) ** 2;
    assert(sse / y.length < 0.05, "the regressor still fits (mse " + sse / y.length + ")");
    const back = GradientBoostingRegressor.deserialize(g.serialize());
    eq(JSON.stringify(back.predict(X)), JSON.stringify(pred),
       "the regressor round-trips bit-identically through the new field");
}

console.log("test_ml_boosting.js: " + n + " assertions passed");
