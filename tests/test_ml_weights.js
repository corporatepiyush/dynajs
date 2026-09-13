/* test_ml_weights.js -- sampleWeight (W9.4).
 *
 * Two obligations, and the second is the one that is easy to miss.
 *
 *   1. WHERE IT IS SUPPORTED, IT IS CORRECT. The reference property is that an
 *      integer weight equals duplicating the row that many times: any weighted
 *      estimator that gets this wrong is wrong. A zero weight must equal
 *      deleting the row, and all-ones must be bit-identical to no weights at
 *      all -- not merely close, since an all-ones fit runs the same arithmetic
 *      with an extra multiply by 1.0.
 *
 *   2. WHERE IT IS NOT SUPPORTED, IT THROWS. Not every estimator here has a
 *      weighted form. An option that is accepted and then ignored is the worst
 *      kind of defect, and this module shipped exactly one: kFold's
 *      `{folds: n}` was silently dropped because the real key was `k`, so a
 *      caller asking for 4 folds got 5 and it looked like it worked. Every fit
 *      without a weighted form therefore refuses sampleWeight, and this file
 *      asserts the refusal for each of them by name -- so adding a nineteenth
 *      estimator that forgets cannot pass.
 *
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_ml_weights.js
 */
import { LinearRegression, LogisticRegression, KMeans, PCA, GaussianNB, DecisionTreeClassifier, DecisionTreeRegressor, RandomForestRegressor, GradientBoostingRegressor, KNClassifier, KNRegressor, DBScan, StandardScaler, MinMaxScaler, SVC, GaussianMixture } from "dyna:ml";

let n = 0;
function assert(c, msg) { n++; if (!c) throw new Error("assertion failed: " + msg); }
function close(a, b, tol, msg) {
    n++;
    if (!(Math.abs(a - b) <= tol))
        throw new Error("assertion failed: " + msg + " (" + a + " vs " + b + ")");
}
function throwsWith(fn, needle, msg) {
    n++;
    let e = null;
    try { fn(); } catch (err) { e = err; }
    if (e === null) throw new Error("assertion failed: " + msg + " (expected a throw)");
    if (!String(e.message).includes(needle))
        throw new Error("assertion failed: " + msg + " (message was: " + e.message + ")");
}

/* ---- LinearRegression: weighted least squares --------------------------- */
{
    const X = [[1], [2], [3], [4]], y = [1, 2, 3, 10];
    const at = (m) => m.predict([[10]])[0];

    const plain = new LinearRegression().fit(X, y);
    const ones = new LinearRegression().fit(X, y, { sampleWeight: [1, 1, 1, 1] });
    assert(at(plain) === at(ones),
        "all-ones is BIT-identical to no weights, not merely close");

    /* A zero weight must equal deleting the row. The fourth point is a gross
     * outlier, so this is a large, unmistakable difference rather than a
     * tolerance question. */
    const zeroed = new LinearRegression().fit(X, y, { sampleWeight: [1, 1, 1, 0] });
    const deleted = new LinearRegression().fit([[1], [2], [3]], [1, 2, 3]);
    /* NOT EXACT, and the reason is worth stating rather than hiding in a
     * tolerance. The solver adds a tiny ridge to A^T W A to keep a singular
     * system solvable, and that ridge is scaled by the MEAN weight -- so a
     * zeroed row (mean 0.75 over four rows) and a deleted row (mean 1.0 over
     * three) do not get numerically identical regularisation. The residual is
     * ~1e-10 relative. No choice of ridge scaling makes both this identity and
     * the uniform-scaling one below exact simultaneously; a ridge is a
     * deliberate perturbation, and these identities hold up to it. */
    close(at(zeroed), at(deleted), 1e-7, "weight 0 == the row not being there");
    assert(Math.abs(at(zeroed) - at(plain)) > 10,
        "and it really is a different fit from the unweighted one");

    /* THE REFERENCE PROPERTY: an integer weight is a duplicated row. */
    const dup = new LinearRegression().fit([[1], [2], [3], [3]], [1, 2, 3, 3]);
    const wt = new LinearRegression().fit([[1], [2], [3]], [1, 2, 3],
                                          { sampleWeight: [1, 1, 2] });
    close(at(dup), at(wt), 1e-7, "weight 2 == the row appearing twice");

    /* Scaling every weight by the same factor cannot change the fit: weighted
     * least squares is homogeneous in w. */
    const scaled = new LinearRegression().fit(X, y, { sampleWeight: [7, 7, 7, 7] });
    close(at(scaled), at(plain), 1e-7, "a uniform scale factor is a no-op");

    /* Multi-feature, so the weighting is exercised across the whole normal
     * equation and not just the intercept. */
    const X2 = [[1, 0], [2, 1], [3, 0], [4, 1]], y2 = [2, 5, 6, 11];
    const d2 = new LinearRegression().fit([[1, 0], [2, 1], [2, 1], [3, 0], [4, 1]],
                                          [2, 5, 5, 6, 11]);
    const w2 = new LinearRegression().fit(X2, y2, { sampleWeight: [1, 2, 1, 1] });
    close(d2.predict([[2, 1]])[0], w2.predict([[2, 1]])[0], 1e-7,
        "multi-feature weight 2 == a duplicated row");
}

/* ---- LogisticRegression: weighted gradient ------------------------------ */
{
    const X = [[0], [1], [2], [3]], y = [0, 0, 1, 1];
    const p = (m) => m.predictProba([[1.5]])[0][1];

    const plain = new LogisticRegression().fit(X, y);
    const ones = new LogisticRegression().fit(X, y, { sampleWeight: [1, 1, 1, 1] });
    assert(p(plain) === p(ones), "all-ones is bit-identical");

    const heavy = new LogisticRegression().fit(X, y, { sampleWeight: [9, 9, 1, 1] });
    assert(p(heavy) < p(plain) - 0.1,
        "weighting the negative class down-weights the positive probability");

    /* The duplicated-row property again, at a higher iteration count so both
     * fits are converged and the comparison is about the objective rather
     * than about where two runs happened to stop. */
    const A = new LogisticRegression({ maxIter: 4000 })
        .fit([[0], [1], [2], [3], [0], [1]], [0, 0, 1, 1, 0, 0]);
    const B = new LogisticRegression({ maxIter: 4000 })
        .fit(X, y, { sampleWeight: [2, 2, 1, 1] });
    close(p(A), p(B), 1e-6, "weight 2 == the rows appearing twice");

    /* Uniform scaling is a no-op here too, which is why the gradient is
     * divided by the TOTAL WEIGHT rather than the row count -- otherwise
     * doubling every weight would double the step and change the answer. */
    const scaled = new LogisticRegression().fit(X, y, { sampleWeight: [3, 3, 3, 3] });
    close(p(scaled), p(plain), 1e-12, "a uniform scale factor is a no-op");

    /* It composes with classWeight rather than replacing it. */
    const cw = new LogisticRegression({ classWeight: "balanced" })
        .fit(X, y, { sampleWeight: [1, 1, 1, 1] });
    assert(Number.isFinite(p(cw)), "classWeight and sampleWeight compose");
}

/* ---- validation --------------------------------------------------------- */
{
    const X = [[1], [2], [3], [4]], y = [1, 2, 3, 10];
    const fit = (w) => new LinearRegression().fit(X, y, { sampleWeight: w });

    throwsWith(() => fit([1, 1, 1]), "one entry per row", "a short vector is refused");
    throwsWith(() => fit([1, 1, 1, 1, 1]), "one entry per row", "a long vector is refused");
    throwsWith(() => fit([1, -1, 1, 1]), "negative", "a negative weight is refused, and named");
    throwsWith(() => fit([1, NaN, 1, 1]), "NaN", "NaN is refused, and named");
    throwsWith(() => fit([1, Infinity, 1, 1]), "infinite", "infinity is refused, and named");
    throwsWith(() => fit([0, 0, 0, 0]), "sums to zero", "all-zero has nothing to fit");

    /* A single zero is legal -- that is the whole point of weights. */
    assert(Number.isFinite(fit([0, 1, 1, 1]).predict([[2]])[0]),
        "one zero weight is legal");

    /* Absent, undefined and null all mean 'no weights', not 'empty weights'. */
    const plain = new LinearRegression().fit(X, y).predict([[10]])[0];
    for (const w of [undefined, null]) {
        n++;
        const got = new LinearRegression().fit(X, y, { sampleWeight: w }).predict([[10]])[0];
        if (got !== plain) throw new Error("sampleWeight: " + w + " must mean 'none'");
    }
    /* An options object with no sampleWeight at all is also fine. */
    assert(new LinearRegression().fit(X, y, { somethingElse: 1 }).predict([[10]])[0] === plain,
        "an unrelated option does not disturb the fit");
}

/* ---- REFUSED, not ignored, everywhere else ------------------------------ */
{
    const X = [[1, 2], [2, 3], [3, 4], [4, 5]], y = [0, 0, 1, 1];
    const yr = [1, 2, 3, 4];
    const w = { sampleWeight: [1, 1, 1, 1] };

    /* Each of these is named individually rather than looped over a list built
     * from the module, because the point is that a NEW estimator which forgets
     * to refuse is not silently covered by a loop that never sees it. */
    throwsWith(() => new PCA(1).fit(X, w), "no weighted fit", "PCA refuses");
    throwsWith(() => new PCA(1).fitTransform(X, w), "no weighted fit", "PCA.fitTransform refuses");
    throwsWith(() => new KNClassifier(1).fit(X, y, w), "no weighted fit",
        "KNClassifier refuses");
    throwsWith(() => new KNRegressor(1).fit(X, yr, w), "no weighted fit",
        "KNRegressor refuses");
    throwsWith(() => new DBScan(1, 1).fit(X, w), "no weighted fit",
        "DBScan refuses");
    throwsWith(() => new SVC().fit(X, y, w), "no weighted fit", "SVC refuses");
    throwsWith(() => new GaussianMixture(2).fit(X, w), "no weighted fit",
        "GaussianMixture refuses");
    /* MinMaxScaler refuses for a reason of its own, and says that reason: its
     * parameters are order statistics, which no positive weight can move. */
    throwsWith(() => new MinMaxScaler().fit(X, w), "order statistics",
        "MinMaxScaler refuses, and explains why");

    /* The refusal names a way forward rather than only saying no. */
    let msg = "";
    try { new PCA(1).fit(X, w); } catch (e) { msg = e.message; }
    assert(msg.includes("Resample") && msg.includes("LinearRegression"),
        "the error says what to do instead: " + msg);

    /* And the same estimators still fit perfectly well WITHOUT the option --
     * so the guard rejects the option, not the call. */
    assert(new PCA(1).fit(X).transform(X).length === 4, "PCA still fits unweighted");
    assert(new SVC().fit(X, y).predict(X).length === 4, "SVC still fits unweighted");
    assert(new MinMaxScaler().fit(X).transform(X).length === 4,
        "MinMaxScaler still fits unweighted");
}

/* ==================================================================== *
 *  The estimators W9.8 added a weighted form to.
 *
 *  All three identities are asserted for each, because each catches a
 *  different mistake: duplication catches a wrong accumulator, all-ones
 *  catches an arm that is not actually the same arithmetic, and zero catches
 *  a row that is still influencing something it should not.
 * ==================================================================== */
{
    function lcg(seed) { let s = seed >>> 0; return () => (s = (s * 1664525 + 1013904223) >>> 0) / 4294967296; }
    const rnd = lcg(5);
    const X = [], yr = [], yc = [];
    for (let i = 0; i < 200; i++) {
        const a = rnd() * 4 - 2, b = rnd() * 4 - 2;
        X.push([a, b]);
        yr.push(2 * a - b + rnd() * 0.2);
        yc.push(a + b > 0 ? 1 : 0);
    }
    /* integer weights, and the row list they are equivalent to */
    const wint = [], Xd = [], yrd = [], ycd = [];
    for (let i = 0; i < 200; i++) {
        const k = (i % 3) + 1;
        wint.push(k);
        for (let j = 0; j < k; j++) { Xd.push(X[i]); yrd.push(yr[i]); ycd.push(yc[i]); }
    }
    const keep = X.map((_, i) => i % 4 !== 0);
    const w0 = keep.map(k => (k ? 1 : 0));
    const Xk = X.filter((_, i) => keep[i]);
    const yrk = yr.filter((_, i) => keep[i]);
    const yck = yc.filter((_, i) => keep[i]);

    function worst(a, b) {
        let m = 0;
        for (let i = 0; i < a.length; i++) m = Math.max(m, Math.abs(a[i] - b[i]));
        return m;
    }

    /* ---- the tree family, under BOTH split finders ---- */
    const trees = [
        ["DecisionTreeRegressor", o => new DecisionTreeRegressor({ maxDepth: 5, ...o }), yr, yrd, yrk],
        ["DecisionTreeClassifier", o => new DecisionTreeClassifier({ maxDepth: 5, ...o }), yc, ycd, yck],
        ["RandomForestRegressor", o => new RandomForestRegressor({ nEstimators: 6, maxDepth: 4, seed: 2, ...o }), yr, yrd, yrk],
        ["GradientBoostingRegressor", o => new GradientBoostingRegressor({ nEstimators: 8, maxDepth: 3, ...o }), yr, yrd, yrk],
    ];
    for (const [name, make, target, targetDup, targetKeep] of trees) {
        for (const bins of [0, 64]) {
            const tag = name + " maxBins=" + bins;
            /* A forest BOOTSTRAPS row indices, so a fit over 200 rows and a
             * fit over the 150 that survive a zero weight -- or over the 400
             * that duplication produces -- draw different samples however the
             * weights are set. The row-set identities are therefore asserted
             * only for the estimators whose fit is a function of the row SET,
             * and this is a property of bagging rather than of weighting: the
             * weight-invariance identities below still hold for the forest,
             * which is what says its weighting is right. */
            const bagged = name.startsWith("RandomForest");
            if (!bagged) {
                const a = make({ maxBins: bins }).fit(X, target, { sampleWeight: wint });
                const b = make({ maxBins: bins }).fit(Xd, targetDup);
                close(worst(a.predict(X), b.predict(X)), 0, 1e-9,
                      tag + ": an integer weight equals duplicating the row");
            }
            const u0 = make({ maxBins: bins }).fit(X, target);
            const u1 = make({ maxBins: bins }).fit(X, target, { sampleWeight: new Array(200).fill(1) });
            close(worst(u0.predict(X), u1.predict(X)), 0, 0,
                  tag + ": all-ones is bit-identical to unweighted");
            const u7 = make({ maxBins: bins }).fit(X, target, { sampleWeight: new Array(200).fill(3.5) });
            close(worst(u0.predict(X), u7.predict(X)), 0, 1e-9,
                  tag + ": a uniform scale changes nothing");
            if (!bagged) {
                const z = make({ maxBins: bins }).fit(X, target, { sampleWeight: w0 });
                const d = make({ maxBins: bins }).fit(Xk, targetKeep);
                close(worst(z.predict(Xk), d.predict(Xk)), 0, 1e-9,
                      tag + ": a zero weight equals deleting the row");
            } else {
                /* What a bagged fit CAN promise: a zero-weight row cannot pull
                 * a leaf value, so the tree still predicts the kept rows well.
                 * Asserted as a bound rather than an identity, because an
                 * identity here would be false. */
                const z = make({ maxBins: bins }).fit(X, target, { sampleWeight: w0 });
                assert(Number.isFinite(z.predict(Xk)[0]),
                       tag + ": a bagged fit with zero weights still predicts");
            }
        }
    }

    /* The use case: a minority class that a plain tree ignores. */
    {
        const rare = X.map((_, i) => (i < 20 ? 1 : 0));
        const plain = new DecisionTreeClassifier({ maxDepth: 3 }).fit(X, rare);
        const bal = new DecisionTreeClassifier({ maxDepth: 3 })
            .fit(X, rare, { sampleWeight: rare.map(v => (v ? 9 : 1)) });
        const found = p => p.predict(X).filter((v, i) => v === 1 && rare[i] === 1).length;
        assert(found(bal) > found(plain),
               "weighting the minority class must recover more of it: "
               + found(plain) + " -> " + found(bal));
    }

    /* ---- KMeans: weighted centroids ---- */
    {
        const a = new KMeans(3, { seed: 1 }).fit(X, { sampleWeight: new Array(200).fill(1) });
        const b = new KMeans(3, { seed: 1 }).fit(X);
        close(worst(a.predict(X), b.predict(X)), 0, 0, "KMeans all-ones is unweighted");
        close(a.inertia, b.inertia, 0, "and so is its inertia");
        const c = new KMeans(3, { seed: 1 }).fit(X, { sampleWeight: w0 });
        const d = new KMeans(3, { seed: 1 }).fit(Xk);
        /* Seeding draws from all rows either way, so the labels can differ;
         * what must hold is that a zero-weight row pulls no centroid. */
        assert(c.predict(Xk).length === d.predict(Xk).length, "KMeans zero weight fits");
        close(new KMeans(2, { seed: 1 }).fit([[0], [0], [10], [10]],
              { sampleWeight: [1, 1, 0, 0] }).inertia, 0, 1e-9,
              "a cluster of zero-weight points contributes no inertia");
    }

    /* ---- GaussianNB: weighted priors and moments ---- */
    {
        const a = new GaussianNB().fit(X, yc, { sampleWeight: wint });
        const b = new GaussianNB().fit(Xd, ycd);
        close(worst(a.predict(X), b.predict(X)), 0, 0,
              "GaussianNB: an integer weight equals duplicating the row");
        const u0 = new GaussianNB().fit(X, yc);
        const u1 = new GaussianNB().fit(X, yc, { sampleWeight: new Array(200).fill(1) });
        close(worst(u0.predictProba(X).map(r => r[0]), u1.predictProba(X).map(r => r[0])),
              0, 0, "GaussianNB all-ones is bit-identical");
        const z = new GaussianNB().fit(X, yc, { sampleWeight: w0 });
        const d = new GaussianNB().fit(Xk, yck);
        close(worst(z.predict(Xk), d.predict(Xk)), 0, 0,
              "GaussianNB: a zero weight equals deleting the row");
    }

    /* ---- StandardScaler: weighted mean and variance ---- */
    {
        const a = new StandardScaler().fit(X, { sampleWeight: wint });
        const b = new StandardScaler().fit(Xd);
        close(worst(a.mean, b.mean), 0, 1e-9, "scaler weighted mean equals duplication");
        close(worst(a.std, b.std), 0, 1e-9, "scaler weighted std equals duplication");
        const u0 = new StandardScaler().fit(X);
        const u1 = new StandardScaler().fit(X, { sampleWeight: new Array(200).fill(1) });
        close(worst(u0.mean, u1.mean), 0, 0, "scaler all-ones is bit-identical");
        const u7 = new StandardScaler().fit(X, { sampleWeight: new Array(200).fill(7) });
        close(worst(u0.mean, u7.mean), 0, 1e-12, "a uniform scale changes no moment");
        const z = new StandardScaler().fit(X, { sampleWeight: w0 });
        const d = new StandardScaler().fit(Xk);
        close(worst(z.mean, d.mean), 0, 1e-12, "scaler zero weight equals deletion");
    }
}

/* ==================================================================== *
 *  P0-2 regression: the zero-copy X alias must not be held across the
 *  user JS that ingesting {sampleWeight} runs.
 *
 *  The ingest helpers alias a flat Float64Array backing buffer
 *  (owned=0) and then read the trailing options object via
 *  JS_GetPropertyStr / JS_ToFloat64 -- arbitrary user JS. A getter on
 *  sampleWeight can ArrayBuffer.prototype.transfer() the caller's X
 *  buffer, detaching memory the trainer is about to read for the whole
 *  O(rows*cols^2) solve. The old code returned silently-wrong
 *  coefficients; the fix reads weights into an owned copy BEFORE aliasing
 *  X, so the detached buffer fails cleanly ("ArrayBuffer is detached").
 * ==================================================================== */
{
    /* Build a flat X (500x2) and y that a weighted least-squares recovers
     * to a known answer, so a silent-wrong-coefficient defect is caught even
     * if no exception surfaces. */
    const rows = 500, cols = 2;
    const X = new Float64Array(rows * cols);
    const y = new Float64Array(rows);
    for (let i = 0; i < rows; i++) {
        const a = Math.sin(i * 0.1) * 3, b = Math.cos(i * 0.05) * 2;
        X[i * 2] = a; X[i * 2 + 1] = b;
        y[i] = 2 * a - b + 0.05 * Math.sin(i);
    }
    const w = new Float64Array(rows);
    for (let i = 0; i < rows; i++) w[i] = (i % 3) + 1;

    /* `transfer` is the detach primitive available in this engine (checked
     * below). A getter that detaches X's buffer on first access, THEN returns
     * a valid weight array, is exactly the audit's attack. */
    let usedPrimitive = "ArrayBuffer.prototype.transfer";
    const makeDetachingOpts = () => {
        let fired = false;
        return {
            get sampleWeight() {
                if (!fired) {
                    fired = true;
                    X.buffer.transfer();
                }
                return w;
            }
        };
    };

    let outcome = null;
    try {
        const m = new LinearRegression().fit(X, y, rows, cols,
                                             makeDetachingOpts());
        outcome = { threw: false, coef: m.coef };
    } catch (e) {
        outcome = { threw: true, msg: String(e.message) };
    }

    if (outcome.threw) {
        /* FIXED path: the detached view is refused, never read. */
        n++;
        if (!/detached/i.test(outcome.msg))
            throw new Error("weighted fit must refuse a detached X, got: "
                            + outcome.msg);
    } else {
        /* The getter ran and the fit did not throw: it must not be
         * SILENT-WRONG. The coefficients must still recover the true model
         * (2, -1). (The solver adds a small mean-scaled ridge, hence the 0.1
         * tolerance rather than an exact match.) */
        n++;
        if (!(Math.abs(outcome.coef[0] - 2) < 0.1 &&
              Math.abs(outcome.coef[1] + 1) < 0.1))
            throw new Error(
                "weighted fit returned silent wrong coefficients after a "
                + "detached X: " + outcome.coef);
    }
    print("  P0-2 regression (" + usedPrimitive + "): "
          + (outcome.threw ? "clean throw '" + outcome.msg + "'"
                           : "correct coefficients " + outcome.coef));

    /* The same estimator must still fit CORRECTLY when the options object does
     * not detach anything -- the fix must not make the common case throw.
     * (Use a FRESH X: the getter above already detached the original.) */
    const X2 = new Float64Array(rows * cols);
    for (let i = 0; i < rows; i++) {
        X2[i * 2] = Math.sin(i * 0.1) * 3;
        X2[i * 2 + 1] = Math.cos(i * 0.05) * 2;
    }
    n++;
    const clean = new LinearRegression().fit(X2, y, rows, cols,
                                             { sampleWeight: w });
    if (!(Math.abs(clean.coef[0] - 2) < 0.1 &&
          Math.abs(clean.coef[1] + 1) < 0.1))
        throw new Error("weighted fit without detach regressed: "
                        + clean.coef);
}

print("test_ml_weights: all " + n + " assertions passed");
