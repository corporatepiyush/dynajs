/* test_ml_trees.js — dyna:ml decision trees, random forests, gradient boosting.
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_ml_trees.js
 * Prints "test_ml_trees: all tests passed" on success; throws on failure. */

import { DecisionTreeClassifier, DecisionTreeRegressor, RandomForestClassifier,
         RandomForestRegressor, GradientBoostingRegressor,
         accuracy, meanSquaredError } from "dyna:ml";

let n = 0;
function assert(cond, msg) { n++; if (!cond) throw new Error("assertion failed: " + msg); }
function approx(a, b, eps, msg) {
    n++;
    if (!(Math.abs(a - b) <= eps)) throw new Error(`approx failed: ${msg} (${a} vs ${b})`);
}
function throws(fn, msg, ErrType) {
    n++;
    try { fn(); } catch (e) {
        if (ErrType && !(e instanceof ErrType))
            throw new Error(`${msg}: wrong error type ${e.constructor.name}: ${e.message}`);
        return;
    }
    throw new Error("expected a throw: " + msg);
}
function rng(seed) {
    let s = seed >>> 0;
    return () => { s = (s * 1664525 + 1013904223) >>> 0; return s / 4294967296; };
}
/* XOR: not linearly separable, so a tree must actually branch twice to get it.
 * This is the fixture that separates a real tree from a single threshold. */
function xorData(perQuadrant, seed) {
    const rnd = rng(seed);
    const X = [], y = [];
    for (const [cx, cy, lab] of [[0, 0, 0], [1, 1, 0], [0, 1, 1], [1, 0, 1]])
        for (let i = 0; i < perQuadrant; i++) {
            X.push([cx + (rnd() - 0.5) * 0.3, cy + (rnd() - 0.5) * 0.3]);
            y.push(lab);
        }
    return { X, y };
}

/* ===================== DecisionTreeClassifier ===================== */
{
    const X = [[1, 1], [1.2, 0.9], [0.8, 1.1], [5, 5], [5.2, 4.8], [4.9, 5.1]];
    const y = [0, 0, 0, 1, 1, 1];
    const m = new DecisionTreeClassifier();
    try {
        assert(m.fit(X, y) === m, "fit returns this");
        const p = m.predict(X);
        for (let i = 0; i < X.length; i++)
            assert(p[i] === y[i], `separable data fitted exactly @${i}`);
        assert(m.depth === 1, "one split suffices here, depth = " + m.depth);
        const q = m.predict([[1.1, 1], [5.1, 5]]);
        assert(q[0] === 0 && q[1] === 1, "unseen points on the right side");
    } finally { m.close(); }
}
{
    /* XOR needs depth >= 2; a stump cannot beat chance on it */
    const { X, y } = xorData(15, 4242);
    const deep = new DecisionTreeClassifier();
    const stump = new DecisionTreeClassifier({ maxDepth: 1 });
    try {
        deep.fit(X, y);
        stump.fit(X, y);
        assert(deep.depth >= 2, "XOR forces depth >= 2, got " + deep.depth);
        assert(stump.depth === 1, "maxDepth caps the depth at 1, got " + stump.depth);
        const accDeep = accuracy(y, deep.predict(X));
        const accStump = accuracy(y, stump.predict(X));
        approx(accDeep, 1, 1e-12, "an unconstrained tree separates XOR exactly");
        assert(accStump <= 0.75, "a stump cannot separate XOR: " + accStump);
        assert(accDeep > accStump, "depth helps on XOR");
    } finally { deep.close(); stump.close(); }
}
{
    /* minSamplesLeaf and minSamplesSplit both stop growth */
    const { X, y } = xorData(20, 99);
    const a = new DecisionTreeClassifier({ minSamplesLeaf: 30 });
    const b = new DecisionTreeClassifier({ minSamplesSplit: 200 });
    try {
        a.fit(X, y);
        b.fit(X, y);
        assert(a.depth <= 2, "a large minSamplesLeaf keeps the tree shallow: " + a.depth);
        assert(b.depth === 0, "minSamplesSplit above the row count gives a single leaf");
        /* a single leaf predicts one constant label for everything */
        const p = b.predict(X);
        assert(p.every((v) => v === p[0]), "a root-only tree is constant");
    } finally { a.close(); b.close(); }
}
{
    /* label VALUES are preserved, not renumbered to 0..k-1 */
    const m = new DecisionTreeClassifier();
    try {
        m.fit([[0], [1], [10], [11]], [-7, -7, 3.5, 3.5]);
        const p = m.predict([[0.5], [10.5]]);
        approx(p[0], -7, 0, "negative label preserved");
        approx(p[1], 3.5, 0, "fractional label preserved");
    } finally { m.close(); }
}

/* ===================== DecisionTreeRegressor ===================== */
{
    /* a step function: the tree should land on the two levels exactly */
    const X = [], y = [];
    for (let i = 0; i < 20; i++) { X.push([i]); y.push(i < 10 ? 5 : 20); }
    const m = new DecisionTreeRegressor({ maxDepth: 1 });
    try {
        m.fit(X, y);
        const p = m.predict([[0], [9], [10], [19]]);
        approx(p[0], 5, 1e-12, "left level");
        approx(p[1], 5, 1e-12, "still left at the boundary");
        approx(p[2], 20, 1e-12, "right level");
        approx(p[3], 20, 1e-12, "right level at the end");
        approx(meanSquaredError(y, m.predict(X)), 0, 1e-12,
               "a step is fitted with zero error by one split");
    } finally { m.close(); }
}
{
    /* a leaf predicts the MEAN of its samples */
    const m = new DecisionTreeRegressor({ maxDepth: 1 });
    try {
        m.fit([[0], [0], [0], [10]], [1, 2, 3, 100]);
        approx(m.predict([[0]])[0], 2, 1e-12, "mean of 1,2,3");
        approx(m.predict([[10]])[0], 100, 1e-12, "the lone right sample");
    } finally { m.close(); }
}

/* ===================== RandomForest ===================== */
{
    /* determinism: the same seed must reproduce the forest exactly, since every
     * stochastic choice (bootstrap, feature subsample) comes from that seed */
    const { X, y } = xorData(25, 31337);
    const a = new RandomForestClassifier({ nEstimators: 15, seed: 7 });
    const b = new RandomForestClassifier({ nEstimators: 15, seed: 7 });
    const c = new RandomForestClassifier({ nEstimators: 15, seed: 8 });
    try {
        a.fit(X, y); b.fit(X, y); c.fit(X, y);
        const pa = a.predict(X), pb = b.predict(X), pc = c.predict(X);
        for (let i = 0; i < X.length; i++)
            assert(pa[i] === pb[i], `same seed -> same prediction @${i}`);
        assert(pc.length === pa.length, "a different seed still predicts every row");
        /* it should still be a good fit regardless of seed */
        assert(accuracy(y, pa) > 0.9, "forest fits XOR: " + accuracy(y, pa));
        assert(accuracy(y, pc) > 0.9, "…with the other seed too: " + accuracy(y, pc));
    } finally { a.close(); b.close(); c.close(); }
}
{
    /* a forest of ONE tree with all features is just a bagged tree; more trees
     * must not make a clean problem worse */
    const { X, y } = xorData(30, 5150);
    const few = new RandomForestClassifier({ nEstimators: 3, seed: 11 });
    const many = new RandomForestClassifier({ nEstimators: 40, seed: 11 });
    try {
        few.fit(X, y); many.fit(X, y);
        const accFew = accuracy(y, few.predict(X));
        const accMany = accuracy(y, many.predict(X));
        assert(accMany >= accFew - 0.05,
               `40 trees are not worse than 3 (${accMany} vs ${accFew})`);
        assert(accMany > 0.9, "40 trees fit well: " + accMany);
    } finally { few.close(); many.close(); }
}
{
    /* regressor: averaging over trees, so predictions land inside the target range */
    const rnd = rng(808);
    const X = [], y = [];
    for (let i = 0; i < 60; i++) {
        const x = rnd() * 10;
        X.push([x, rnd()]);
        y.push(3 * x + 2 + (rnd() - 0.5) * 0.5);
    }
    const m = new RandomForestRegressor({ nEstimators: 25, seed: 3 });
    try {
        m.fit(X, y);
        const p = m.predict(X);
        const lo = Math.min(...y), hi = Math.max(...y);
        for (let i = 0; i < p.length; i++)
            assert(p[i] >= lo - 1e-9 && p[i] <= hi + 1e-9,
                   `averaged prediction stays in the target range @${i}`);
        /* it must beat predicting the global mean */
        const mean = y.reduce((s, v) => s + v, 0) / y.length;
        const mseModel = meanSquaredError(y, p);
        const mseMean = meanSquaredError(y, y.map(() => mean));
        assert(mseModel < mseMean * 0.2,
               `forest beats the mean baseline (${mseModel} vs ${mseMean})`);
    } finally { m.close(); }
}
{
    /* maxFeatures = 1 still trains (each split sees one random feature) */
    const { X, y } = xorData(20, 1212);
    const m = new RandomForestClassifier({ nEstimators: 30, maxFeatures: 1, seed: 5 });
    try {
        m.fit(X, y);
        assert(accuracy(y, m.predict(X)) > 0.85,
               "maxFeatures=1 still learns XOR by ensembling");
    } finally { m.close(); }
}

/* ===================== GradientBoostingRegressor ===================== */
{
    const rnd = rng(6161);
    const X = [], y = [];
    for (let i = 0; i < 80; i++) {
        const x = rnd() * 6 - 3;
        X.push([x]);
        y.push(x * x);                        /* a curve no single stump fits */
    }
    const one = new GradientBoostingRegressor({ nEstimators: 1, maxDepth: 2 });
    const many = new GradientBoostingRegressor({ nEstimators: 120, maxDepth: 2 });
    try {
        one.fit(X, y); many.fit(X, y);
        const mse1 = meanSquaredError(y, one.predict(X));
        const mseN = meanSquaredError(y, many.predict(X));
        assert(mseN < mse1, `boosting reduces error with more rounds (${mseN} < ${mse1})`);
        assert(mseN < 0.5, "120 rounds fit x^2 closely: " + mseN);
        /* with 0 additional signal the base prediction is the target mean */
        const flat = new GradientBoostingRegressor({ nEstimators: 5 });
        try {
            flat.fit([[1], [1], [1]], [4, 6, 8]);
            approx(flat.predict([[1]])[0], 6, 1e-9,
                   "a constant feature leaves only the base mean");
        } finally { flat.close(); }
    } finally { one.close(); many.close(); }
}
{
    /* learningRate scales each tree's contribution: a tiny rate stays near the
     * base prediction after few rounds */
    const X = [], y = [];
    for (let i = 0; i < 20; i++) { X.push([i]); y.push(i < 10 ? 0 : 100); }
    const slow = new GradientBoostingRegressor({ nEstimators: 2, learningRate: 0.001, maxDepth: 1 });
    const fast = new GradientBoostingRegressor({ nEstimators: 2, learningRate: 1.0, maxDepth: 1 });
    try {
        slow.fit(X, y); fast.fit(X, y);
        const base = 50;                       /* mean of the targets */
        assert(Math.abs(slow.predict([[19]])[0] - base) < 1,
               "learningRate 0.001 barely moves off the base");
        assert(Math.abs(fast.predict([[19]])[0] - 100) < 1,
               "learningRate 1.0 reaches the target in one step");
    } finally { slow.close(); fast.close(); }
}
{
    /* subsample < 1 is stochastic but seed-reproducible */
    const rnd = rng(4242);
    const X = [], y = [];
    for (let i = 0; i < 50; i++) { const x = rnd() * 4; X.push([x]); y.push(Math.sin(x)); }
    const a = new GradientBoostingRegressor({ nEstimators: 20, subsample: 0.6, seed: 21, maxDepth: 2 });
    const b = new GradientBoostingRegressor({ nEstimators: 20, subsample: 0.6, seed: 21, maxDepth: 2 });
    try {
        a.fit(X, y); b.fit(X, y);
        const pa = a.predict(X), pb = b.predict(X);
        for (let i = 0; i < pa.length; i++)
            approx(pa[i], pb[i], 0, `subsampled fit is seed-reproducible @${i}`);
    } finally { a.close(); b.close(); }
}

/* ===================== flat form and options validation ===================== */
{
    const rowsArr = [[1, 1], [1.2, 0.9], [5, 5], [5.2, 4.8]];
    const flat = new Float64Array([1, 1, 1.2, 0.9, 5, 5, 5.2, 4.8]);
    const y = [0, 0, 1, 1];
    const a = new DecisionTreeClassifier(), b = new DecisionTreeClassifier();
    try {
        a.fit(rowsArr, y);
        b.fit(flat, new Float64Array(y), 4, 2);
        const pa = a.predict(rowsArr), pb = b.predict(flat, 4, 2);
        for (let i = 0; i < 4; i++)
            assert(pa[i] === pb[i], "Array and flat forms agree @" + i);
    } finally { a.close(); b.close(); }
}
{
    throws(() => new RandomForestClassifier({ nEstimators: 0 }), "nEstimators >= 1", RangeError);
    throws(() => new DecisionTreeClassifier({ minSamplesSplit: 1 }), "minSamplesSplit >= 2", RangeError);
    throws(() => new DecisionTreeClassifier({ minSamplesLeaf: 0 }), "minSamplesLeaf >= 1", RangeError);
    throws(() => new GradientBoostingRegressor({ subsample: 0 }), "subsample > 0", RangeError);
    throws(() => new GradientBoostingRegressor({ subsample: 2 }), "subsample <= 1", RangeError);
    throws(() => new DecisionTreeClassifier(42), "options must be an object", TypeError);
    /* unknown keys are ignored rather than rejected, for forward compatibility */
    const m = new DecisionTreeClassifier({ someFutureKnob: 5 });
    try {
        m.fit([[1], [2]], [0, 1]);
        assert(m.predict([[1]])[0] === 0, "unknown option ignored, model still trains");
    } finally { m.close(); }
}
{
    const m = new RandomForestRegressor({ nEstimators: 2 });
    try {
        throws(() => m.predict([[1]]), "predict before fit");
        assert(m.depth === 0, "depth is 0 before fit");
        m.fit([[1], [2], [3]], [1, 2, 3]);
        throws(() => m.predict([[1, 2]]), "wrong feature count", TypeError);
        throws(() => m.fit([[1], [2]], [1]), "y length mismatch", TypeError);
    } finally { m.close(); }
}

/* ===================== reentrancy ===================== */
{
    const m = new DecisionTreeClassifier();
    throws(() => m.fit([[{ valueOf() { m.close(); return 1; } }], [2]], [0, 1]),
        "reentrant close during tree fit");
    assert(m.closed === true, "closed");
}
{
    const m = new RandomForestClassifier({ nEstimators: 3 });
    m.fit([[1], [2], [3], [4]], [0, 0, 1, 1]);
    throws(() => m.predict([[{ valueOf() { m.close(); return 1; } }]]),
        "reentrant close during forest predict");
    assert(m.closed === true, "closed");
}
{
    const m = new GradientBoostingRegressor({ nEstimators: 3 });
    throws(() => m.fit([[{ valueOf() { m.close(); return 1; } }], [2]], [0, 1]),
        "reentrant close during boosting fit");
    assert(m.closed === true, "closed");
}

print("test_ml_trees: all tests passed (" + n + " assertions)");
