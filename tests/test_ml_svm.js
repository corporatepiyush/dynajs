/* test_ml_svm.js — dyna:ml SVC (SMO) + GaussianMixture (EM).
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_ml_svm.js
 * Prints "test_ml_svm: all tests passed" on success; throws on failure. */

import { SVC, GaussianMixture, accuracy } from "dyna:ml";

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
function xorData(perQuadrant, seed) {
    const rnd = rng(seed);
    const X = [], y = [];
    for (const [cx, cy, lab] of [[0, 0, 0], [1, 1, 0], [0, 1, 1], [1, 0, 1]])
        for (let i = 0; i < perQuadrant; i++) {
            X.push([cx + (rnd() - 0.5) * 0.2, cy + (rnd() - 0.5) * 0.2]);
            y.push(lab);
        }
    return { X, y };
}

/* ===================== SVC: linearly separable ===================== */
{
    const X = [[1, 1], [1.2, 0.9], [0.8, 1.1], [5, 5], [5.2, 4.8], [4.9, 5.1]];
    const y = [0, 0, 0, 1, 1, 1];
    for (const kernel of ["linear", "rbf", "poly"]) {
        const m = new SVC({ kernel, C: 10 });
        try {
            assert(m.fit(X, y) === m, `${kernel}: fit returns this`);
            approx(accuracy(y, m.predict(X)), 1, 0, `${kernel}: separable data fitted exactly`);
            const p = m.predict([[1.1, 1], [5.1, 5]]);
            assert(p[0] === 0 && p[1] === 1, `${kernel}: unseen points classified`);
            /* only a subset of points should end up as support vectors */
            assert(m.nSupportVectors > 0, `${kernel}: has support vectors`);
            assert(m.nSupportVectors <= X.length,
                   `${kernel}: no more support vectors than samples`);
            const cls = m.classes;
            assert(cls.length === 2 && cls[0] === 0 && cls[1] === 1,
                   `${kernel}: classes ascending`);
        } finally { m.close(); }
    }
}
{
    /* decisionFunction and predict must agree: the sign of the margin IS the
     * decision, so a disagreement would mean one of them is wrong */
    const X = [[0], [1], [8], [9]];
    const y = [0, 0, 1, 1];
    const m = new SVC({ kernel: "linear", C: 10 });
    try {
        m.fit(X, y);
        const q = [[0.5], [1.5], [7], [8.5], [4.5]];
        const d = m.decisionFunction(q);
        const p = m.predict(q);
        assert(Array.isArray(d) && d.length === 5, "binary decisionFunction is one value per row");
        for (let i = 0; i < q.length; i++)
            assert((d[i] >= 0 ? 1 : 0) === p[i],
                   `sign of the margin matches the label @${i} (d=${d[i]}, p=${p[i]})`);
        /* the margin should be monotone along this 1-D problem */
        assert(d[0] < d[1] && d[1] < d[4] && d[4] < d[2] && d[2] < d[3],
               "margin increases towards the positive class");
    } finally { m.close(); }
}
{
    /* XOR: this is what the kernel is FOR. A linear SVC cannot separate it; an
     * RBF SVC can. If both passed, the kernel would not be doing anything. */
    const { X, y } = xorData(15, 2024);
    const lin = new SVC({ kernel: "linear", C: 10 });
    const rbf = new SVC({ kernel: "rbf", C: 10, gamma: 1.0 });
    try {
        lin.fit(X, y);
        rbf.fit(X, y);
        const accLin = accuracy(y, lin.predict(X));
        const accRbf = accuracy(y, rbf.predict(X));
        assert(accRbf > 0.95, "RBF separates XOR: " + accRbf);
        assert(accLin < 0.8, "linear cannot separate XOR: " + accLin);
        assert(accRbf > accLin, "the kernel is what makes the difference");
    } finally { lin.close(); rbf.close(); }
}
{
    /* multi-class one-vs-rest: three well-separated groups */
    const X = [[0, 0], [0.1, 0], [0, 0.1], [5, 5], [5.1, 5], [5, 5.1],
               [10, 0], [10.1, 0], [10, 0.1]];
    const y = [0, 0, 0, 1, 1, 1, 2, 2, 2];
    const m = new SVC({ kernel: "rbf", C: 100, gamma: 0.1 });
    try {
        m.fit(X, y);
        approx(accuracy(y, m.predict(X)), 1, 0, "three classes fitted exactly");
        assert(m.classes.length === 3, "three classes discovered");
        const d = m.decisionFunction([[0, 0], [5, 5]]);
        assert(Array.isArray(d) && Array.isArray(d[0]) && d[0].length === 3,
               "multi-class decisionFunction is rows x classes");
        /* the winning column must be the argmax, consistent with predict */
        const probe = [[0, 0], [5, 5]];
        const probePred = m.predict(probe);
        for (let i = 0; i < probe.length; i++) {
            let best = 0;
            for (let c = 1; c < 3; c++) if (d[i][c] > d[i][best]) best = c;
            assert(probePred[i] === m.classes[best],
                   `argmax of the OvR values is the prediction @${i}`);
        }
    } finally { m.close(); }
}
{
    /* non-integer, non-contiguous labels survive */
    const m = new SVC({ kernel: "linear", C: 10 });
    try {
        m.fit([[0], [0.2], [9], [9.2]], [-2.5, -2.5, 7, 7]);
        const p = m.predict([[0.1], [9.1]]);
        approx(p[0], -2.5, 0, "negative fractional label preserved");
        approx(p[1], 7, 0, "positive label preserved");
    } finally { m.close(); }
}
{
    /* C controls the margin/violation trade-off; both extremes must still train */
    const { X, y } = xorData(10, 77);
    for (const C of [0.01, 1000]) {
        const m = new SVC({ kernel: "rbf", C, gamma: 1 });
        try {
            m.fit(X, y);
            const p = m.predict(X);
            assert(p.length === X.length, `C=${C} still predicts every row`);
            assert(p.every((v) => v === 0 || v === 1), `C=${C} predicts valid labels`);
        } finally { m.close(); }
    }
}
{
    throws(() => new SVC({ kernel: "sigmoid" }), "unsupported kernel name", TypeError);
    throws(() => new SVC({ C: 0 }), "C must be positive", RangeError);
    throws(() => new SVC({ degree: 0 }), "degree at least 1", RangeError);
    throws(() => new SVC({ maxIter: 0 }), "maxIter at least 1", RangeError);
    throws(() => new SVC(7), "options must be an object", TypeError);
    const m = new SVC();
    try {
        throws(() => m.predict([[1]]), "predict before fit");
        assert(m.nSupportVectors === 0, "no support vectors before fit");
        throws(() => m.fit([[1], [2]], [3, 3]), "a single class cannot be separated", RangeError);
        m.fit([[1], [2]], [0, 1]);
        throws(() => m.predict([[1, 2]]), "wrong feature count", TypeError);
    } finally { m.close(); }
}

/* ===================== GaussianMixture ===================== */
{
    /* two tight, well-separated blobs of equal size */
    const rnd = rng(31337);
    const X = [];
    for (let i = 0; i < 40; i++) X.push([rnd() * 0.4, rnd() * 0.4]);
    for (let i = 0; i < 40; i++) X.push([10 + rnd() * 0.4, 10 + rnd() * 0.4]);
    const g = new GaussianMixture(2, { seed: 5 });
    try {
        assert(g.fit(X) === g, "fit returns this");
        const w = g.weights;
        assert(w.length === 2, "two weights");
        approx(w[0] + w[1], 1, 1e-12, "weights sum to 1");
        approx(w[0], 0.5, 0.05, "equal-size blobs give equal weights");
        /* means land on the blob centres (in some order) */
        const means = g.means;
        assert(means.length === 2 && means[0].length === 2, "means are k x cols");
        const near0 = means.filter((mu) => Math.abs(mu[0]) < 1).length;
        const near10 = means.filter((mu) => Math.abs(mu[0] - 10) < 1).length;
        assert(near0 === 1 && near10 === 1, "one mean per blob");
        /* hard labels split the blobs */
        const L = g.predict(X);
        assert(L.slice(0, 40).every((v) => v === L[0]), "first blob is one component");
        assert(L.slice(40).every((v) => v === L[40]), "second blob is one component");
        assert(L[0] !== L[40], "the blobs get different components");
        /* responsibilities are a probability distribution per row */
        const P = g.predictProba(X);
        for (let i = 0; i < X.length; i++) {
            approx(P[i][0] + P[i][1], 1, 1e-12, `responsibilities sum to 1 @${i}`);
            assert(P[i][0] >= 0 && P[i][0] <= 1, `responsibility in range @${i}`);
        }
        /* and they are confident, since the blobs are far apart */
        assert(Math.max(P[0][0], P[0][1]) > 0.99, "confident assignment for a clear point");
        assert(g.nIter >= 1 && g.nIter <= 200, "iteration count reported: " + g.nIter);
        assert(Number.isFinite(g.logLikelihood), "log likelihood is finite");
        assert(g.variances.length === 2, "variances are k x cols");
        for (const v of g.variances)
            for (const vj of v) assert(vj > 0, "every variance is strictly positive");
    } finally { g.close(); }
}
{
    /* EM increases the log likelihood: more iterations must not make it worse */
    const rnd = rng(4242);
    const X = [];
    for (let i = 0; i < 60; i++) X.push([rnd() * 3, rnd() * 3]);
    for (let i = 0; i < 60; i++) X.push([6 + rnd() * 3, 6 + rnd() * 3]);
    const one = new GaussianMixture(2, { seed: 9, maxIter: 1, tol: 0 });
    const many = new GaussianMixture(2, { seed: 9, maxIter: 60, tol: 0 });
    try {
        one.fit(X);
        many.fit(X);
        assert(many.nIter > one.nIter, "the second model ran more iterations");
        assert(many.logLikelihood >= one.logLikelihood - 1e-9,
               `EM does not decrease the likelihood (${many.logLikelihood} vs ${one.logLikelihood})`);
    } finally { one.close(); many.close(); }
}
{
    /* determinism from the seed */
    const rnd = rng(11);
    const X = [];
    for (let i = 0; i < 50; i++) X.push([rnd() * 5, rnd() * 5, rnd() * 5]);
    const a = new GaussianMixture(3, { seed: 21 });
    const b = new GaussianMixture(3, { seed: 21 });
    try {
        a.fit(X); b.fit(X);
        for (let c = 0; c < 3; c++) {
            approx(a.weights[c], b.weights[c], 0, `weight ${c} is bit-identical`);
            for (let j = 0; j < 3; j++)
                approx(a.means[c][j], b.means[c][j], 0, `mean ${c}[${j}] is bit-identical`);
        }
        approx(a.logLikelihood, b.logLikelihood, 0, "same log likelihood");
    } finally { a.close(); b.close(); }
}
{
    /* k = 1: the single component is just the data's mean and variance */
    const X = [[1, 10], [2, 20], [3, 30], [4, 40]];
    const g = new GaussianMixture(1, { seed: 1 });
    try {
        g.fit(X);
        approx(g.weights[0], 1, 1e-12, "the only weight is 1");
        approx(g.means[0][0], 2.5, 1e-9, "mean of column 0");
        approx(g.means[0][1], 25, 1e-9, "mean of column 1");
        /* population variance of 1,2,3,4 is 1.25; regCovar adds 1e-6 */
        approx(g.variances[0][0], 1.25, 1e-4, "variance of column 0");
        assert(g.predict(X).every((v) => v === 0), "everything is component 0");
        approx(g.predictProba(X)[0][0], 1, 1e-12, "its responsibility is 1");
    } finally { g.close(); }
}
{
    /* DUPLICATE points: a component collapsing onto a single location would give
     * zero variance and an infinite density. regCovar must keep it finite. */
    const X = [];
    for (let i = 0; i < 10; i++) X.push([5, 5]);
    for (let i = 0; i < 10; i++) X.push([9, 9]);
    const g = new GaussianMixture(2, { seed: 3, regCovar: 1e-6 });
    try {
        g.fit(X);
        assert(Number.isFinite(g.logLikelihood),
               "log likelihood stays finite on degenerate data: " + g.logLikelihood);
        for (const v of g.variances)
            for (const vj of v) assert(vj > 0 && Number.isFinite(vj),
                                       "variances stay positive and finite");
        const P = g.predictProba(X);
        for (let i = 0; i < X.length; i++)
            approx(P[i][0] + P[i][1], 1, 1e-12, `responsibilities still normalised @${i}`);
    } finally { g.close(); }
}
{
    /* flat form and shape-in shape-out */
    const flat = new Float64Array([0, 0, 0.1, 0.1, 10, 10, 10.1, 10.1]);
    const g = new GaussianMixture(2, { seed: 4 });
    try {
        g.fit(flat, 4, 2);
        const P = g.predictProba(flat, 4, 2);
        assert(P instanceof Float64Array, "flat in -> Float64Array out");
        assert(P.length === 8, "4 rows x 2 components");
        approx(P[0] + P[1], 1, 1e-12, "row 0 of the flat result sums to 1");
        const L = g.predict(flat, 4, 2);
        assert(L[0] === L[1] && L[2] === L[3] && L[0] !== L[2],
               "flat form clusters the pairs");
    } finally { g.close(); }
}
{
    throws(() => new GaussianMixture(0), "nComponents at least 1", RangeError);
    throws(() => new GaussianMixture(2, { maxIter: 0 }), "maxIter at least 1", RangeError);
    throws(() => new GaussianMixture(2, { regCovar: -1 }), "regCovar not negative", RangeError);
    throws(() => new GaussianMixture(2, 5), "options must be an object", TypeError);
    const g = new GaussianMixture(3);
    try {
        throws(() => g.predict([[1]]), "predict before fit");
        assert(g.weights.length === 0, "no weights before fit");
        assert(g.nIter === 0, "no iterations before fit");
        throws(() => g.fit([[1], [2]]), "fewer rows than components", RangeError);
        g.fit([[1], [2], [3], [4]]);
        throws(() => g.predict([[1, 2]]), "wrong feature count", TypeError);
        /* refitting must not leak the previous parameters */
        g.fit([[5], [6], [7], [8]]);
        assert(g.weights.length === 3, "refit still has three components");
    } finally { g.close(); }
}

/* ===================== reentrancy ===================== */
{
    const m = new SVC({ kernel: "linear" });
    throws(() => m.fit([[{ valueOf() { m.close(); return 1; } }], [2]], [0, 1]),
        "reentrant close during SVC fit");
    assert(m.closed === true, "closed");
}
{
    const m = new SVC({ kernel: "linear", C: 10 });
    m.fit([[1], [2]], [0, 1]);
    throws(() => m.decisionFunction([[{ valueOf() { m.close(); return 1; } }]]),
        "reentrant close during decisionFunction");
    assert(m.closed === true, "closed");
}
{
    const g = new GaussianMixture(2);
    throws(() => g.fit([[{ valueOf() { g.close(); return 1; } }], [2], [3]]),
        "reentrant close during GMM fit");
    assert(g.closed === true, "closed");
}
{
    const g = new GaussianMixture(2, { seed: 1 });
    g.fit([[0], [0.1], [9], [9.1]]);
    throws(() => g.predictProba([[{ valueOf() { g.close(); return 1; } }]]),
        "reentrant close during predictProba");
    assert(g.closed === true, "closed");
}

/* --- Cap and NaN-predict refusals (audit batch, 2026-08) ------------------ */
{
    const X = [[0, 0], [0, 1], [1, 0], [1, 1], [2, 0], [2, 1]];
    const y = [0, 0, 0, 1, 1, 1];
    const throwsRange = (fn, msg) => {
        n++;
        try { fn(); } catch (e) {
            if (!(e instanceof RangeError))
                throw new Error(msg + ": wrong error " + e.constructor.name);
            return;
        }
        throw new Error("expected a throw: " + msg);
    };
    throwsRange(() => new SVC({ degree: 2000 }), "svc degree cap");
    throwsRange(() => new SVC({ degree: 2 ** 31 }), "svc degree overflow cap");
    throwsRange(() => new SVC({ maxIter: 100001 }), "svc maxIter cap");
    const m = new SVC({ maxIter: 500 }); m.fit(X, y);
    throwsRange(() => m.predict([[NaN, 0]]), "svc predict NaN refusal");
    throwsRange(() => new GaussianMixture(2, { maxIter: 10001 }), "gmm maxIter cap");
    const g = new GaussianMixture(2, { seed: 3 }); g.fit(X);
    throwsRange(() => g.predict([[NaN, 0]]), "gmm predict NaN refusal");
}

/* --- class cap, poly overflow, GMM refit preservation (2nd audit batch) --- */
{
    throws(() => {
        const m = new SVC({ maxIter: 100 });
        m.fit(Array.from({ length: 257 }, (_, i) => [i]),
              Array.from({ length: 257 }, (_, i) => i));
    }, "svc class-count cap", RangeError);
    throws(() => {
        const p = new SVC({ kernel: "poly", degree: 100, gamma: 1, coef0: 1 });
        p.fit([[1e6, 1e6], [2e6, 2e6], [1e6, 2e6], [2e6, 1e6]], [0, 0, 1, 1]);
    }, "svc poly kernel overflow refusal", RangeError);
    /* a failed refit must leave the previous model usable */
    const gX = [[0, 0], [0, 1], [1, 0], [1, 1], [2, 0], [2, 1]];
    const g = new GaussianMixture(2, { seed: 3 }); g.fit(gX);
    throws(() => g.fit([[1.0]]), "gmm refit with rows < k", RangeError);
    assert(g.predict(gX).length === gX.length, "failed refit preserves the model");
}

print("test_ml_svm: all tests passed (" + n + " assertions)");
