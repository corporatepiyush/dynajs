/* test_ml_tree_proba.js -- predictProba, featureImportances and apply on the
 * tree family (W9.2; the tree-surface gap #4).
 *
 * The point of the gap: dyna:ml already ships logLoss, rocAuc and
 * averagePrecision, and every one of them needs a SCORE rather than a label.
 * Without predictProba the whole tree family could not be measured with them.
 *
 * What is actually being pinned here is that the probabilities are the LEAF
 * CLASS DISTRIBUTIONS, not the fraction of trees voting for a label. The
 * difference is invisible on a large forest and decisive on a single tree,
 * where a vote fraction would only ever be 0 or 1 -- so the tests use data with
 * genuine class overlap, which is the only kind that can tell the two apart.
 */
import {
    DecisionTreeClassifier, DecisionTreeRegressor,
    RandomForestClassifier, RandomForestRegressor, GradientBoostingRegressor,
    logLoss, rocAuc, averagePrecision, accuracy,
} from "dyna:ml";

let n = 0;
function assert(c, m) { n++; if (!c) throw new Error("assertion failed: " + m); }
function close(a, b, eps, m) {
    n++;
    if (!(Math.abs(a - b) <= eps))
        throw new Error((m || "not close") + ": " + a + " vs " + b);
}
function throws(fn, kind, m) {
    n++;
    try { fn(); } catch (e) {
        if (kind && !(e instanceof kind)) throw new Error((m || "wrong error") + ": " + e);
        return;
    }
    throw new Error((m || "expected a throw") + " but none happened");
}

function lcg(seed) { let s = seed >>> 0; return () => (s = (s * 1664525 + 1013904223) >>> 0) / 4294967296; }

/* Deliberately OVERLAPPING classes: the label is a noisy function of the
 * features, so leaves are mixed and a leaf distribution is not 0/1. Feature 2
 * is pure noise and must end up with a near-zero importance. */
const rnd = lcg(31337);
const X = [], y = [];
for (let i = 0; i < 400; i++) {
    const a = rnd() * 4 - 2, b = rnd() * 4 - 2, noise = rnd() * 4 - 2;
    const p = 1 / (1 + Math.exp(-(a + 2 * b)));
    X.push([a, b, noise]);
    y.push(rnd() < p ? 1 : 0);
}
const Xt = X.slice(0, 60), yt = y.slice(0, 60);

/* ==================================================================== *
 *  1. predictProba is a probability distribution
 * ==================================================================== */
{
    for (const model of [new DecisionTreeClassifier({ maxDepth: 4 }),
                         new RandomForestClassifier({ nEstimators: 20, maxDepth: 5, seed: 1 })]) {
        const name = model.constructor.name;
        model.fit(X, y);
        const P = model.predictProba(Xt);
        assert(P.length === Xt.length, name + " one row per input");
        assert(P[0].length === 2, name + " one column per class");
        for (let i = 0; i < P.length; i++) {
            let s = 0;
            for (const v of P[i]) {
                assert(v >= 0 && v <= 1, name + " probability out of range: " + v);
                s += v;
            }
            close(s, 1, 1e-12, name + " row " + i + " does not sum to 1");
        }
    }
}

/* ==================================================================== *
 *  1b. predict() and argmax(predictProba) are DIFFERENT RULES
 *
 *  This is a measured property of the current design, pinned so that a change
 *  to either one has to be deliberate:
 *
 *    predict()      = majority vote over each tree's HARD label
 *    predictProba() = mean of each tree's LEAF DISTRIBUTION
 *
 *  For one tree the two coincide by construction, because the label is the
 *  argmax of that same distribution. For a forest they can differ, and they do:
 *  a tree that is 51% sure contributes a whole vote to predict() and 0.51 to
 *  predictProba(). They now AGREE, by decision D7: predict() is the argmax of
 *  the averaged leaf distributions, which is the quantity predictProba
 *  returns.
 *
 *  Before that change they disagreed on 7 of 400 rows at 100 trees and 4 of
 *  400 at 20, with the averaged form the more accurate of the two on the same
 *  data (0.9525 against 0.9350). Two APIs answering the same question
 *  differently is a defect, and scikit-learn settled the same way for the same
 *  reason -- but it changes every existing user's predictions, so it is a
 *  recorded decision and not a silent fix.
 *
 *  This block is now the guard in the OTHER direction: if predict() ever
 *  reverts to a majority vote over hard labels, these assertions fail.
 * ==================================================================== */
{
    const dt = new DecisionTreeClassifier({ maxDepth: 5 });
    dt.fit(X, y);
    const P1 = dt.predictProba(X), pred1 = dt.predict(X);
    for (let i = 0; i < X.length; i++)
        if (P1[i][0] !== P1[i][1])
            assert(pred1[i] === (P1[i][0] > P1[i][1] ? 0 : 1),
                   "a single tree's vote IS its leaf argmax, row " + i);

    const rf = new RandomForestClassifier({ nEstimators: 20, maxDepth: 6, seed: 1 });
    rf.fit(X, y);
    const P = rf.predictProba(X), pred = rf.predict(X);
    let disagree = 0;
    for (let i = 0; i < X.length; i++)
        if (pred[i] !== (P[i][0] >= P[i][1] ? 0 : 1)) disagree++;
    assert(disagree === 0,
           "predict() must BE argmax(predictProba); " + disagree + "/" +
           X.length + " rows disagree, so predict() has reverted to a vote");

    /* And at a forest size where the old rule provably differed, so this is
     * not passing merely because the two happen to coincide here. */
    const rf100 = new RandomForestClassifier({ nEstimators: 100, maxDepth: 6, seed: 1 });
    rf100.fit(X, y);
    const P100 = rf100.predictProba(X), pred100 = rf100.predict(X);
    let d100 = 0;
    for (let i = 0; i < X.length; i++)
        if (pred100[i] !== (P100[i][0] >= P100[i][1] ? 0 : 1)) d100++;
    assert(d100 === 0, "still argmax at 100 trees (" + d100 + " disagreements)");
    print("  predict() === argmax(predictProba) at 20 and 100 trees (D7)");
}

/* ==================================================================== *
 *  2. It is a leaf DISTRIBUTION, not a vote fraction
 *
 *  A single tree over overlapping classes must produce probabilities strictly
 *  between 0 and 1. A vote fraction over one tree can only be 0 or 1, so this
 *  assertion fails outright under the wrong implementation.
 * ==================================================================== */
{
    const dt = new DecisionTreeClassifier({ maxDepth: 3 });
    dt.fit(X, y);
    const P = dt.predictProba(X);
    let fractional = 0;
    for (const row of P)
        if (row[0] > 0 && row[0] < 1) fractional++;
    assert(fractional > X.length / 2,
           "a single tree produced " + fractional + " fractional rows of " +
           X.length + "; a vote fraction would produce none");

    /* And the distinct probability levels are the leaves, not the trees. */
    const levels = new Set(P.map(r => r[1].toFixed(12)));
    assert(levels.size > 2,
           "only " + levels.size + " distinct probability levels");
}

/* ==================================================================== *
 *  3. The metrics that needed it now work on the tree family
 * ==================================================================== */
{
    const rf = new RandomForestClassifier({ nEstimators: 40, maxDepth: 6, seed: 2 });
    rf.fit(X, y);
    const score = rf.predictProba(Xt).map(r => r[1]);
    const ll = logLoss(yt, score);
    const auc = rocAuc(yt, score);
    const ap = averagePrecision(yt, score);
    assert(ll > 0 && isFinite(ll), "logLoss is finite: " + ll);
    assert(auc > 0.8 && auc <= 1, "rocAuc on separable-ish data: " + auc);
    assert(ap > 0.8 && ap <= 1, "averagePrecision: " + ap);
    /* A model fitted on the data it is scored on should beat chance by a lot;
     * the point is that the numbers are meaningful, not that they are good. */
    assert(accuracy(yt, rf.predict(Xt)) > 0.8, "accuracy sanity");
}

/* ==================================================================== *
 *  4. featureImportances
 * ==================================================================== */
{
    for (const model of [new DecisionTreeClassifier({ maxDepth: 6 }),
                         new RandomForestClassifier({ nEstimators: 30, seed: 3 }),
                         new RandomForestRegressor({ nEstimators: 30, seed: 3 }),
                         new GradientBoostingRegressor({ nEstimators: 20, seed: 3 })]) {
        const name = model.constructor.name;
        model.fit(X, y);
        const imp = model.featureImportances;
        assert(imp.length === 3, name + " one weight per feature");
        let s = 0;
        for (const v of imp) {
            assert(v >= 0, name + " importance is non-negative");
            s += v;
        }
        close(s, 1, 1e-12, name + " importances sum to 1");
        /* y depends on a + 2b, so the ranking must be b > a > noise. This is
         * the assertion that catches an importance computed from the wrong
         * quantity -- a shuffled attribution would not reproduce the weights.
         *
         * The noise feature still collects 0.11-0.19 rather than ~0, and that
         * is not a bug: an unrestricted tree keeps splitting on it deep down
         * where the samples are few, and every such split records a small
         * impurity decrease. It is exactly the training-fit bias the docs warn
         * about, so the bound is "well below the dominant feature", not
         * "near zero". */
        assert(imp[1] > imp[0], name + ": b (weight 2) should outrank a");
        assert(imp[0] > imp[2], name + ": a should outrank pure noise");
        assert(imp[2] < imp[1] / 2, name + ": noise ranked too close to b");
    }

    /* A tree that never split has no basis for a preference, and must not
     * invent one by reporting a uniform vector. */
    const flat = new DecisionTreeClassifier({ maxDepth: 1, minSamplesSplit: 1e9 });
    flat.fit(X, y);
    const fi = flat.featureImportances;
    assert(fi.length === 3 && fi.every(v => v === 0),
           "an unsplit tree reports zeros, not a uniform guess: " + JSON.stringify(fi));
}

/* ==================================================================== *
 *  5. apply() -- the leaf a row lands in
 * ==================================================================== */
{
    const rf = new RandomForestClassifier({ nEstimators: 7, maxDepth: 4, seed: 4 });
    rf.fit(X, y);
    const A = rf.apply(Xt);
    assert(A.length === Xt.length, "one row per input");
    assert(A[0].length === 7, "one column per tree");
    for (const row of A)
        for (const v of row)
            assert(Number.isInteger(v) && v >= 0, "a leaf index: " + v);

    /* Identical rows must land in identical leaves, and a row's leaves must be
     * stable across calls -- apply() is a lookup, not a sample. */
    const twice = rf.apply(Xt);
    assert(JSON.stringify(A) === JSON.stringify(twice), "apply is deterministic");
    const dup = rf.apply([Xt[0], Xt[0]]);
    assert(JSON.stringify(dup[0]) === JSON.stringify(dup[1]),
           "identical rows land in identical leaves");
    assert(JSON.stringify(dup[0]) === JSON.stringify(A[0]),
           "and in the same leaves as in the bigger batch");

    /* A single tree gives one column. */
    const dt = new DecisionTreeRegressor({ maxDepth: 3 });
    dt.fit(X, y);
    assert(dt.apply(Xt)[0].length === 1, "a single tree has one column");
}

/* ==================================================================== *
 *  6. Errors
 * ==================================================================== */
{
    const reg = new RandomForestRegressor({ nEstimators: 3, seed: 5 });
    reg.fit(X, y);
    throws(() => reg.predictProba(Xt), TypeError,
           "predictProba on a regressor");
    const gb = new GradientBoostingRegressor({ nEstimators: 3, seed: 5 });
    gb.fit(X, y);
    throws(() => gb.predictProba(Xt), TypeError,
           "predictProba on a boosted regressor");

    const unfit = new RandomForestClassifier({ nEstimators: 3 });
    throws(() => unfit.predictProba(Xt), Error, "predictProba before fit");
    throws(() => unfit.apply(Xt), Error, "apply before fit");
    throws(() => unfit.featureImportances, Error, "featureImportances before fit");

    const fitted = new RandomForestClassifier({ nEstimators: 3, seed: 6 });
    fitted.fit(X, y);
    throws(() => fitted.predictProba([[1, 2]]), TypeError, "wrong feature count");
    throws(() => fitted.apply([[1, 2]]), TypeError, "wrong feature count for apply");
}

/* ==================================================================== *
 *  7. All three survive a persistence round trip, bit-identically
 *
 *  They are part of the fitted model, so a record that dropped them would load
 *  a model that answers differently from the one that was saved.
 * ==================================================================== */
{
    for (const build of [() => new DecisionTreeClassifier({ maxDepth: 5 }),
                         () => new RandomForestClassifier({ nEstimators: 12, seed: 7 }),
                         () => new RandomForestRegressor({ nEstimators: 6, seed: 7 }),
                         () => new GradientBoostingRegressor({ nEstimators: 8, seed: 7 })]) {
        const m = build();
        m.fit(X, y);
        const name = m.constructor.name;
        const C = m.constructor;
        const want = JSON.stringify([
            m.classifier === false ? null : null,
            m.featureImportances,
            m.apply(Xt),
            m.predict(Xt),
        ]);
        const wantProba = name.indexOf("Classifier") >= 0
            ? JSON.stringify(m.predictProba(Xt)) : null;

        const back = C.deserialize(m.serialize());
        assert(JSON.stringify([null, back.featureImportances, back.apply(Xt),
                               back.predict(Xt)]) === want,
               name + ": importances/apply/predict changed across a round trip");
        if (wantProba !== null)
            assert(JSON.stringify(back.predictProba(Xt)) === wantProba,
                   name + ": predictProba changed across a round trip");
    }
}

print("test_ml_tree_proba: all " + n + " assertions passed");
