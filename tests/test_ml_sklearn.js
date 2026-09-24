/* test_ml_sklearn.js -- the external value oracle for dyna:ml.
 *
 * Every expected value in tests/test_ml_sklearn_vectors.js was FITTED BY
 * SCIKIT-LEARN (the version in that file's header) on the deterministic
 * datasets shipped beside it -- not recorded from this engine. A round trip
 * proves a codec agrees with itself; this proves the models agree with the
 * reference implementation. Tolerances are per-family and named, and are the
 * reason each comparison can pass: optimizer differences (GD vs lbfgs) get
 * loose bounds, closed-form families (NB, scalers, linear least squares) get
 * tight ones.
 *
 * Run: dynajs (CONFIG_NATIVE_MODULES=y) tests/test_ml_sklearn.js */
import * as ml from "dyna:ml";
import { DS, E } from "./test_ml_sklearn_vectors.js";

let n = 0;
function assert(c, m) { n++; if (!c) throw new Error("assertion failed: " + m); }
function eq(got, want, m) {
    n++;
    if (got !== want) throw new Error("assertion failed: " + m +
        "\n  got:  " + got + "\n  want: " + want);
}
function near(got, want, tol, m) {
    n++;
    if (!(Math.abs(got - want) <= tol))
        throw new Error("assertion failed: " + m +
            "\n  got:  " + got + "\n  want: " + want + "\n  tol:   " + tol);
}
function maxdiff(a, b) {
    let d = 0;
    for (let i = 0; i < a.length; i++) d = Math.max(d, Math.abs(a[i] - b[i]));
    return d;
}
const F = (arr) => new Float64Array(arr);
const rowsOf = (flat, cols) => Array.from({ length: flat.length / cols },
    (_, i) => Array.from(flat.slice(i * cols, (i + 1) * cols)));
/* predictProba honours shape-in-shape-out per estimator: nested for array
 * input, flat for flat input, and logreg is nested either way. */
const cell = (m, i, k, cols) => Array.isArray(m[i]) ? m[i][k] : m[i * cols + k];

const T = (name, cols) => {
    const X = F(DS[name][0]);
    const y = DS[name][1] ? F(DS[name][1]) : null;
    return { X, y, rows: X.length / cols, cols };
};

/* ---------- LinearRegression: normal equations vs lstsq ------------------- */
{
    const { X, y, rows, cols } = T("lin", 3);
    const m = new ml.LinearRegression().fit(X, y, rows, cols);
    assert(maxdiff(m.coef, E.lin.coef) <= 1e-6, "linreg coef vs sklearn");
    near(m.intercept, E.lin.intercept, 1e-6, "linreg intercept vs sklearn");
    assert(maxdiff(m.predict(X, rows, cols), E.lin.pred) <= 1e-6,
           "linreg predict vs sklearn");
}

/* ---------- LogisticRegression: full-batch GD vs lbfgs -------------------- */
{
    const { X, y, rows, cols } = T("clf", 3);
    /* sklearn default C=1.0 (l2); pin the same regulariser so the optimum is
     * unique -- an unregularised fit on near-separable data has none, and two
     * optimisers then "disagree" by drifting apart, which proves nothing */
    const m = new ml.LogisticRegression({ penalty: "l2", C: 1.0, tol: 1e-8,
                                          maxIter: 20000 })
        .fit(X, y, rows, cols);
    const proba = m.predictProba(X, rows, cols);
    let d = 0;
    for (let i = 0; i < rows; i++)
        for (let k = 0; k < 2; k++)
            d = Math.max(d, Math.abs(cell(proba, i, k, 2) - E.log.proba[i * 2 + k]));
    assert(d <= 1e-6, "logreg proba vs sklearn (maxdiff " + d + ")");
    const pred = m.predict(X, rows, cols);
    let fence = 0;
    for (let i = 0; i < rows; i++) {
        const p = E.log.proba[i * 2 + 1];
        if (Math.abs(p - 0.5) < 0.05) { fence++; continue; }
        eq(pred[i], E.log.pred[i], "logreg label vs sklearn @" + i);
    }
    assert(fence < rows / 5, "logreg fence rows: " + fence + " (data not separable)");
}

/* ---------- KMeans: inertia is permutation-invariant ---------------------- */
{
    const { X, rows, cols } = T("km", 2);
    const m = new ml.KMeans(3, { seed: 42 }).fit(X, rows, cols);
    near(m.inertia, E.km.inertia, Math.max(1e-6, 1e-6 * Math.abs(E.km.inertia)),
         "kmeans inertia vs sklearn");
}

/* ---------- PCA: variance exact, components up to sign -------------------- */
{
    const { X, rows, cols } = T("pca", 3);
    const m = new ml.PCA(2).fit(X, rows, cols);
    assert(maxdiff(m.explainedVariance, E.pca.explained) <= 1e-9,
           "pca explained variance vs sklearn");
    const t = m.transform(X, rows, cols);           /* flat rows x 2 */
    for (let c = 0; c < 2; c++) {
        let plus = 0, minus = 0;
        for (let i = 0; i < rows; i++) {
            const got = t[i * 2 + c], want = E.pca.transform[i * 2 + c];
            plus = Math.max(plus, Math.abs(got - want));
            minus = Math.max(minus, Math.abs(got + want));
        }
        assert(Math.min(plus, minus) <= 1e-9,
               "pca component " + c + " vs sklearn up to sign");
    }
}

/* ---------- GaussianNB: closed form, tight --------------------------------- */
{
    const { X, y, rows, cols } = T("nb", 3);
    const m = new ml.GaussianNB().fit(X, y, rows, cols);
    {
        const proba = m.predictProba(X, rows, cols);
        let d = 0;
        for (let i = 0; i < rows; i++)
            for (let k = 0; k < 2; k++)
                d = Math.max(d, Math.abs(cell(proba, i, k, 2) - E.nb.proba[i * 2 + k]));
        assert(d <= 1e-9, "nb proba vs sklearn (maxdiff " + d + ")");
    }
    assert(maxdiff(m.predict(X, rows, cols), E.nb.pred) === 0,
           "nb labels vs sklearn");
}

/* ---------- scalers: closed form, tight ------------------------------------ */
{
    const { X, rows, cols } = T("scale", 3);
    const ss = new ml.StandardScaler().fit(X, rows, cols);
    assert(maxdiff(ss.mean, E.ss.mean) <= 1e-12, "std scaler mean vs sklearn");
    assert(maxdiff(ss.std, E.ss.scale) <= 1e-12, "std scaler scale vs sklearn");
    assert(maxdiff(ss.transform(X, rows, cols), E.ss.transform) <= 1e-12,
           "std scaler transform vs sklearn");
    const mm = new ml.MinMaxScaler().fit(X, rows, cols);
    assert(maxdiff(mm.transform(X, rows, cols), E.mm.transform) <= 1e-12,
           "minmax transform vs sklearn");
}

/* ---------- SVC: labels on cleanly separable data -------------------------- */
{
    const { X, y, rows, cols } = T("svc", 2);
    /* sklearn's default gamma is 1/(n_features * var); this engine's default
     * is 1/cols -- pin gamma explicitly so the kernels are identical and the
     * comparison is about the classifier, not the default. */
    let v = 0;
    for (let i = 0; i < X.length; i++) v += X[i] * X[i];
    v /= X.length;
    const m = new ml.SVC({ kernel: "rbf", C: 1.0, gamma: 1 / (2 * v) })
        .fit(X, y, rows, cols);
    assert(maxdiff(m.predict(X, rows, cols), E.svc.pred) === 0,
           "svc labels vs sklearn");
}

/* ---------- trees: greedy CART, thresholds are deterministic --------------- */
{
    {
        const { X, y, rows, cols } = T("tree", 3);
        const m = new ml.DecisionTreeRegressor({ maxDepth: 4 })
            .fit(X, y, rows, cols);
        assert(maxdiff(m.predict(X, rows, cols), E.dtr.pred) <= 1e-9,
               "tree regressor predict vs sklearn");
    }
    {
        const { X, y, rows, cols } = T("tc", 3);
        const m = new ml.DecisionTreeClassifier({ maxDepth: 4 })
            .fit(X, y, rows, cols);
        assert(maxdiff(m.predict(X, rows, cols), E.dtc.pred) === 0,
               "tree classifier labels vs sklearn");
        {
            const proba = m.predictProba(X, rows, cols);
            let d = 0;
            for (let i = 0; i < rows; i++)
                for (let k = 0; k < 2; k++)
                    d = Math.max(d, Math.abs(cell(proba, i, k, 2) - E.dtc.proba[i * 2 + k]));
            assert(d <= 1e-9, "tree classifier proba vs sklearn (maxdiff " + d + ")");
        }
    }
}

/* ---------- GaussianMixture: EM, same optimum on well-separated blobs ------- */
{
    const { X, rows, cols } = T("gmm", 2);
    const m = new ml.GaussianMixture(3, { regCovar: 1e-6, seed: 42 })
        .fit(X, rows, cols);
    near(m.logLikelihood, E.gmm.ll, 1e-6 * Math.abs(E.gmm.ll),
         "gmm log-likelihood vs sklearn");
}

/* ---------- KNN: brute-force, exact ----------------------------------------- */
{
    const { X, y, rows, cols } = T("knn", 3);
    const c = new ml.KNClassifier(5).fit(X, y, rows, cols);
    assert(maxdiff(c.predict(X, rows, cols), E.knn.clf) === 0,
           "knn classifier labels vs sklearn");
    const r = new ml.KNRegressor(5).fit(X, y, rows, cols);
    assert(maxdiff(r.predict(X, rows, cols), E.knn.reg) <= 1e-12,
           "knn regressor values vs sklearn");
}

/* ---------- metrics: same formulas, tight ----------------------------------- */
{
    const yt = [1, 1, 1, 0, 0, 0, 0, 0, 0, 0];
    const ys = [0.9, 0.8, 0.7, 0.6, 0.5, 0.4, 0.3, 0.2, 0.1, 0.05];
    const yp2 = [0.95, 0.85, 0.65, 0.55, 0.45, 0.35, 0.25, 0.15, 0.15, 0.05];
    const ypred = [1, 1, 0, 1, 0, 0, 0, 0, 0, 0];
    near(ml.r2Score(yt, yp2), E.metrics.r2, 1e-12, "r2 vs sklearn");
    near(ml.meanAbsoluteError(yt, ys), E.metrics.mae, 1e-12, "mae vs sklearn");
    near(ml.meanSquaredError(yt, ys), E.metrics.mse, 1e-12, "mse vs sklearn");
    near(ml.logLoss(yt, [0.9, 0.8, 0.7, 0.4, 0.3, 0.2, 0.1, 0.05, 0.15, 0.02]),
         E.metrics.logloss, 1e-12, "logLoss vs sklearn");
    near(ml.accuracy(yt, ypred), E.metrics.acc, 1e-12, "accuracy vs sklearn");
    near(ml.rocAuc(yt, ys), E.metrics.auc, 1e-12, "rocAuc vs sklearn");
    near(ml.averagePrecision(yt, ys), E.metrics.ap, 1e-12,
         "averagePrecision vs sklearn");
    const cm = ml.confusionMatrix(yt, ypred);
    const flat = [];
    for (const row of cm) for (const v of row) flat.push(v);
    assert(maxdiff(flat, E.metrics.conf) === 0, "confusionMatrix vs sklearn");
}

print("test_ml_sklearn: all " + n + " comparisons passed");
