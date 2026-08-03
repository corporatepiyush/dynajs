/* test_ml_decomposition.js — dyna:ml PCA + GaussianNB.
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_ml_decomposition.js
 * Prints "test_ml_decomposition: all tests passed" on success; throws on failure. */

import { PCA, GaussianNB, StandardScaler } from "dyna:ml";

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

/* ===================== PCA: a known 2-D case ===================== */
{
    /* points on the line y = x, so all variance is along (1,1)/sqrt(2) and the
     * second component carries none */
    const X = [[-2, -2], [-1, -1], [0, 0], [1, 1], [2, 2]];
    const p = new PCA();
    try {
        assert(p.fit(X) === p, "fit returns this");
        approx(p.mean[0], 0, 1e-12, "mean[0]");
        approx(p.mean[1], 0, 1e-12, "mean[1]");
        const c = p.components;
        assert(c.length === 2 && c[0].length === 2, "2 components of 2 features");
        const r = Math.SQRT1_2;
        /* sign convention: the largest-magnitude entry is positive */
        approx(Math.abs(c[0][0]), r, 1e-9, "|component 0 [0]| = 1/sqrt(2)");
        approx(Math.abs(c[0][1]), r, 1e-9, "|component 0 [1]| = 1/sqrt(2)");
        assert(c[0][0] > 0 || c[0][1] > 0, "the leading entry is made positive");
        /* unit norm and orthogonality are structural, not incidental */
        approx(c[0][0] * c[0][0] + c[0][1] * c[0][1], 1, 1e-9, "component 0 is a unit vector");
        approx(c[1][0] * c[1][0] + c[1][1] * c[1][1], 1, 1e-9, "component 1 is a unit vector");
        approx(c[0][0] * c[1][0] + c[0][1] * c[1][1], 0, 1e-9, "components are orthogonal");
        /* variance: sample variance along the line is 2*var(x) = 2*2.5 = 5 */
        approx(p.explainedVariance[0], 5, 1e-9, "leading eigenvalue");
        approx(p.explainedVariance[1], 0, 1e-9, "second eigenvalue is 0 (degenerate data)");
        approx(p.explainedVarianceRatio[0], 1, 1e-9, "the first component explains everything");
        approx(p.explainedVarianceRatio[1], 0, 1e-9, "the second explains nothing");
    } finally { p.close(); }
}
{
    /* deterministic sign: the same data fitted twice gives identical components */
    const X = [[3, -1], [1, 4], [-2, 2], [5, 5], [0, -3]];
    const a = new PCA(), b = new PCA();
    try {
        a.fit(X); b.fit(X);
        for (let i = 0; i < 2; i++)
            for (let j = 0; j < 2; j++)
                approx(a.components[i][j], b.components[i][j], 0,
                       `component ${i}[${j}] is bit-identical across fits`);
    } finally { a.close(); b.close(); }
}
{
    /* round trip: with ALL components kept, inverseTransform(transform(X)) === X */
    const X = [[1, 2, 3], [4, 5, 7], [-1, 0, 2], [3, -2, 1], [0, 4, -3]];
    const p = new PCA();
    try {
        const t = p.fitTransform(X);
        assert(t.length === 5 && t[0].length === 3, "5x3 projection");
        const back = p.inverseTransform(t);
        for (let i = 0; i < 5; i++)
            for (let j = 0; j < 3; j++)
                approx(back[i][j], X[i][j], 1e-8, `round trip [${i}][${j}]`);
        /* the projection is centred: every column sums to ~0 */
        for (let j = 0; j < 3; j++) {
            let s = 0;
            for (let i = 0; i < 5; i++) s += t[i][j];
            approx(s, 0, 1e-8, `projected column ${j} is centred`);
        }
        /* the projected columns are uncorrelated - the point of PCA */
        for (let a2 = 0; a2 < 3; a2++)
            for (let b2 = a2 + 1; b2 < 3; b2++) {
                let cov = 0;
                for (let i = 0; i < 5; i++) cov += t[i][a2] * t[i][b2];
                approx(cov, 0, 1e-7, `projected columns ${a2},${b2} are uncorrelated`);
            }
        /* eigenvalues come out descending */
        const ev = p.explainedVariance;
        for (let a2 = 1; a2 < ev.length; a2++)
            assert(ev[a2] <= ev[a2 - 1] + 1e-12, "eigenvalues are descending");
        /* the ratios sum to 1 */
        approx(p.explainedVarianceRatio.reduce((s, v) => s + v, 0), 1, 1e-9,
               "explained variance ratios sum to 1");
        /* each projected column's sample variance IS its eigenvalue */
        for (let a2 = 0; a2 < 3; a2++) {
            let ss = 0;
            for (let i = 0; i < 5; i++) ss += t[i][a2] * t[i][a2];
            approx(ss / 4, ev[a2], 1e-8, `projected variance ${a2} = eigenvalue`);
        }
    } finally { p.close(); }
}
{
    /* dimensionality reduction keeps only the leading components */
    const X = [[1, 2, 3], [4, 5, 7], [-1, 0, 2], [3, -2, 1], [0, 4, -3], [2, 2, 2]];
    const p = new PCA(2);
    try {
        const t = p.fitTransform(X);
        assert(t[0].length === 2, "projected to 2 columns");
        assert(p.components.length === 2, "2 components stored");
        assert(p.explainedVariance.length === 2, "2 eigenvalues");
        /* the ratio no longer sums to 1, but each entry is still a fraction of
         * the TOTAL variance, so the sum is below 1 and above 0 */
        const s = p.explainedVarianceRatio.reduce((a, v) => a + v, 0);
        assert(s > 0 && s < 1.0000001, "kept ratio is a fraction of the total: " + s);
        /* reconstruction is now lossy but still centred correctly */
        const back = p.inverseTransform(t);
        assert(back[0].length === 3, "inverse returns full feature width");
    } finally { p.close(); }
}
{
    /* whiten: projected columns get unit variance */
    const X = [[-2, 1], [-1, 3], [0, 0], [1, -3], [2, -1], [3, 2]];
    const p = new PCA(2, true);
    try {
        const t = p.fitTransform(X);
        for (let a2 = 0; a2 < 2; a2++) {
            let ss = 0;
            for (let i = 0; i < 6; i++) ss += t[i][a2] * t[i][a2];
            approx(ss / 5, 1, 1e-7, `whitened column ${a2} has unit variance`);
        }
    } finally { p.close(); }
}
{
    /* flat form: shape in = shape out */
    const flat = new Float64Array([-2, -2, -1, -1, 0, 0, 1, 1, 2, 2]);
    const p = new PCA(1);
    try {
        const t = p.fitTransform(flat, 5, 2);
        assert(t instanceof Float64Array, "flat in -> Float64Array out");
        assert(t.length === 5, "5 rows x 1 component");
        /* projections along y=x are +-sqrt(2)*|x| */
        approx(Math.abs(t[0]), 2 * Math.SQRT2, 1e-9, "extreme point projects farthest");
        approx(t[2], 0, 1e-9, "the centre point projects to 0");
    } finally { p.close(); }
}
{
    const p = new PCA(5);
    try {
        throws(() => p.fit([[1, 2], [3, 4]]), "more components than features", RangeError);
    } finally { p.close(); }
    const q = new PCA();
    try {
        throws(() => q.fit([[1, 2]]), "fewer than two rows", RangeError);
        throws(() => q.transform([[1, 2]]), "transform before fit");
        q.fit([[1, 2], [3, 5]]);
        throws(() => q.transform([[1, 2, 3]]), "wrong feature count", TypeError);
    } finally { q.close(); }
    throws(() => new PCA(-1), "negative nComponents", RangeError);
}

/* ===================== GaussianNB ===================== */
{
    /* two clearly separated classes */
    const X = [[1, 1], [1.2, 0.9], [0.8, 1.1], [5, 5], [5.2, 4.8], [4.9, 5.1]];
    const y = [0, 0, 0, 1, 1, 1];
    const m = new GaussianNB();
    try {
        assert(m.fit(X, y) === m, "fit returns this");
        const cls = m.classes;
        assert(cls.length === 2 && cls[0] === 0 && cls[1] === 1, "classes ascending");
        const p = m.predict([[1, 1], [5, 5], [1.1, 1]]);
        assert(p[0] === 0 && p[1] === 1 && p[2] === 0, "separated classes predicted");
        const pr = m.predictProba([[1, 1], [5, 5]]);
        assert(pr.length === 2 && pr[0].length === 2, "proba is rows x classes");
        for (const row of pr)
            approx(row[0] + row[1], 1, 1e-12, "each probability row sums to 1");
        assert(pr[0][0] > 0.99, "confident about class 0");
        assert(pr[1][1] > 0.99, "confident about class 1");
    } finally { m.close(); }
}
{
    /* non-contiguous, non-integer labels must be preserved, not renumbered */
    const m = new GaussianNB();
    try {
        m.fit([[0], [0.1], [9], [9.1]], [7, 7, -3.5, -3.5]);
        const cls = m.classes;
        assert(cls.length === 2, "two classes");
        approx(cls[0], -3.5, 0, "classes are sorted ascending: -3.5 first");
        approx(cls[1], 7, 0, "and 7 second");
        const p = m.predict([[0], [9]]);
        approx(p[0], 7, 0, "predicts the original label value, not an index");
        approx(p[1], -3.5, 0, "and the negative label too");
    } finally { m.close(); }
}
{
    /* THREE classes, and many features: this is the case that underflows if the
     * likelihood is computed as a product instead of a sum of logs */
    const rows = 60, cols = 40;
    const X = [], y = [];
    let s = 12345;
    const rnd = () => { s = (s * 1664525 + 1013904223) >>> 0; return s / 4294967296; };
    for (let i = 0; i < rows; i++) {
        const c = i % 3;
        const row = [];
        for (let j = 0; j < cols; j++) row.push(c * 10 + rnd());
        X.push(row); y.push(c);
    }
    const m = new GaussianNB();
    try {
        m.fit(X, y);
        const p = m.predict(X);
        let correct = 0;
        for (let i = 0; i < rows; i++) if (p[i] === y[i]) correct++;
        assert(correct === rows, `all ${rows} training points classified, got ${correct}`);
        const pr = m.predictProba(X);
        for (let i = 0; i < rows; i++) {
            let sum = 0;
            for (let c = 0; c < 3; c++) {
                assert(Number.isFinite(pr[i][c]), `proba finite at [${i}][${c}]`);
                assert(pr[i][c] >= 0 && pr[i][c] <= 1, `proba in range at [${i}][${c}]`);
                sum += pr[i][c];
            }
            approx(sum, 1, 1e-12, `row ${i} sums to 1 with 40 features`);
        }
    } finally { m.close(); }
}
{
    /* a feature that is CONSTANT within a class has zero variance: smoothing must
     * keep the density finite instead of dividing by zero */
    const m = new GaussianNB();
    try {
        m.fit([[1, 5], [1, 6], [2, 50], [2, 60]], [0, 0, 1, 1]);
        const p = m.predict([[1, 5], [2, 55]]);
        assert(p[0] === 0 && p[1] === 1, "still classifies with a constant feature");
        const pr = m.predictProba([[1, 5]]);
        assert(Number.isFinite(pr[0][0]) && Number.isFinite(pr[0][1]),
               "probabilities stay finite (no 1/0)");
        approx(pr[0][0] + pr[0][1], 1, 1e-12, "and still normalised");
    } finally { m.close(); }
}
{
    /* a single class is degenerate but must not crash: everything is that class */
    const m = new GaussianNB();
    try {
        m.fit([[1], [2], [3]], [4, 4, 4]);
        assert(m.classes.length === 1, "one class");
        const p = m.predict([[1], [100]]);
        approx(p[0], 4, 0, "everything is the only class");
        approx(p[1], 4, 0, "even far from the data");
        approx(m.predictProba([[100]])[0][0], 1, 1e-12, "its probability is 1");
    } finally { m.close(); }
}
{
    const m = new GaussianNB();
    try {
        throws(() => m.predict([[1]]), "predict before fit");
        m.fit([[1], [2]], [0, 1]);
        throws(() => m.predict([[1, 2]]), "wrong feature count", TypeError);
        throws(() => m.fit([[1], [2]], [0]), "y length mismatch", TypeError);
    } finally { m.close(); }
    throws(() => new GaussianNB(-1), "negative varSmoothing", RangeError);
}

/* ===================== pipeline: scaler -> PCA -> NB ===================== */
{
    /* the three new pieces composed, which is how they will actually be used */
    const X = [[100, 0.1], [110, 0.12], [90, 0.09], [500, 0.9], [520, 0.95], [480, 0.85]];
    const y = [0, 0, 0, 1, 1, 1];
    const sc = new StandardScaler(), p = new PCA(1), nb = new GaussianNB();
    try {
        const z = sc.fitTransform(X);            /* features on wildly different scales */
        const t = p.fitTransform(z);             /* down to one component */
        nb.fit(t, y);
        const pred = nb.predict(t);
        for (let i = 0; i < 6; i++)
            assert(pred[i] === y[i], `pipeline classifies training point ${i}`);
        /* and an unseen point on each side */
        const q = p.transform(sc.transform([[105, 0.11], [510, 0.9]]));
        const qp = nb.predict(q);
        assert(qp[0] === 0 && qp[1] === 1, "pipeline classifies unseen points");
    } finally { sc.close(); p.close(); nb.close(); }
}

/* ===================== reentrancy ===================== */
{
    const p = new PCA();
    throws(() => p.fit([[{ valueOf() { p.close(); return 1; } }, 2], [3, 4]]),
        "reentrant close during PCA fit");
    assert(p.closed === true, "closed");
}
{
    const m = new GaussianNB();
    m.fit([[1], [2]], [0, 1]);
    throws(() => m.predictProba([[{ valueOf() { m.close(); return 1; } }]]),
        "reentrant close during predictProba");
    assert(m.closed === true, "closed");
}

print("test_ml_decomposition: all tests passed (" + n + " assertions)");
