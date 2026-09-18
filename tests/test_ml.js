/* test_ml.js — dyna:ml (in-repo LinearRegression / LogisticRegression / KMeans).
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_ml.js
 * Prints "test_ml: all tests passed" on success; throws on failure. */

import { LinearRegression, LogisticRegression, KMeans } from "dyna:ml";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}
function approx(a, b, eps, msg) {
    n++;
    if (Math.abs(a - b) > eps)
        throw new Error("approx failed: " + msg + " (" + a + " vs " + b + ")");
}

/* --- LinearRegression: recover y = 2x + 1 --------------------------------- */
{
    const m = new LinearRegression();
    try {
        const X = [], y = [];
        for (let x = 0; x < 20; x++) { X.push([x]); y.push(2 * x + 1); }
        assert(m.fit(X, y) === m, "fit returns this");
        const p = m.predict([[100], [0], [-3]]);
        assert(Array.isArray(p) && p.length === 3, "predict returns Array of 3");
        approx(p[0], 201, 1e-4, "y(100)=201");
        approx(p[1], 1, 1e-4, "y(0)=1");
        approx(p[2], -5, 1e-4, "y(-3)=-5");
    } finally { m.close(); }
}

/* --- LinearRegression: multivariate plane z = 3a - 2b + 5 ----------------- */
{
    const m = new LinearRegression();
    try {
        const X = [], y = [];
        for (let a = 0; a < 6; a++)
            for (let b = 0; b < 6; b++) { X.push([a, b]); y.push(3 * a - 2 * b + 5); }
        m.fit(X, y);
        const p = m.predict([[10, 10], [0, 0]]);
        approx(p[0], 3 * 10 - 2 * 10 + 5, 1e-3, "plane at (10,10)");
        approx(p[1], 5, 1e-3, "plane intercept");
    } finally { m.close(); }
}

/* --- LogisticRegression: separable 2-class toy set ------------------------ */
{
    const m = new LogisticRegression();
    try {
        // class 0 clusters near (0,0), class 1 near (5,5)
        const X = [], y = [];
        const c0 = [[0, 0], [1, 0], [0, 1], [1, 1], [0.5, 0.5]];
        const c1 = [[5, 5], [4, 5], [5, 4], [4, 4], [4.5, 4.5]];
        for (const p of c0) { X.push(p); y.push(0); }
        for (const p of c1) { X.push(p); y.push(1); }
        m.fit(X, y);
        const labels = m.predict([[0, 0], [5, 5], [1, 0.5], [4.5, 5]]);
        assert(labels[0] === 0, "origin -> class 0");
        assert(labels[1] === 1, "far corner -> class 1");
        assert(labels[2] === 0, "near-origin -> class 0");
        assert(labels[3] === 1, "near-far -> class 1");
        /* rows x n_classes since W9.5: column k is P(classes[k]|row), which is
         * the shape every other classifier here already returned. */
        const proba = m.predictProba([[0, 0], [5, 5]]);
        assert(proba[0][1] >= 0 && proba[0][1] < 0.5, "P(class1|origin) < 0.5");
        assert(proba[1][1] > 0.5 && proba[1][1] <= 1, "P(class1|far) > 0.5");
        assert(Math.abs(proba[0][0] + proba[0][1] - 1) < 1e-12, "row sums to 1");
    } finally { m.close(); }
}

/* --- KMeans: two well-separated blobs ------------------------------------- */
{
    const m = new KMeans(2, 7);
    try {
        const X = [
            [0, 0], [0.2, -0.1], [-0.1, 0.2], [0.1, 0.1],
            [10, 10], [10.2, 9.9], [9.8, 10.1], [10.1, 10.0],
        ];
        assert(m.fit(X) === m, "fit returns this");
        const labels = m.predict(X);
        // first four share one label, last four share the other, and differ.
        const a = labels[0], b = labels[4];
        assert(a !== b, "the two blobs get different clusters");
        for (let i = 0; i < 4; i++) assert(labels[i] === a, "blob A cohesive @" + i);
        for (let i = 4; i < 8; i++) assert(labels[i] === b, "blob B cohesive @" + i);
        assert(typeof m.inertia === "number", "inertia is a number");
        assert(m.inertia >= 0 && m.inertia < 1, "tight blobs => small inertia");
        // a fresh point near blob B classifies with blob B
        const q = m.predict([[9.9, 10.0]]);
        assert(q[0] === b, "query near B -> B");
    } finally { m.close(); }
}

/* --- default KMeans inertia is 0 before fit ------------------------------- */
{
    const m = new KMeans();
    try {
        assert(m.inertia === 0, "inertia 0 before fit");
    } finally { m.close(); }
}

/* --- dimension-mismatch throws -------------------------------------------- */
{
    const m = new LinearRegression();
    try {
        let threw = false;
        try { m.fit([[1], [2]], [1]); } catch { threw = true; }
        assert(threw, "y length != rows throws");
        threw = false;
        try { m.fit([[1, 2], [3]], [1, 2]); } catch { threw = true; }
        assert(threw, "ragged X rows throw");
        m.fit([[1], [2], [3]], [3, 5, 7]);
        threw = false;
        try { m.predict([[1, 2]]); } catch { threw = true; }
        assert(threw, "predict feature-count mismatch throws");
        threw = false;
        try { m.fit([], []); } catch { threw = true; }
        assert(threw, "empty X throws");
    } finally { m.close(); }
}

/* --- predict-before-fit throws -------------------------------------------- */
{
    const m = new LogisticRegression();
    try {
        let threw = false;
        try { m.predict([[1, 2]]); } catch { threw = true; }
        assert(threw, "predict before fit throws");
    } finally { m.close(); }
}

/* --- KMeans needs at least nClusters rows --------------------------------- */
{
    const m = new KMeans(5);
    try {
        let threw = false;
        try { m.fit([[1], [2]]); } catch { threw = true; }
        assert(threw, "fewer rows than clusters throws");
    } finally { m.close(); }
}

/* --- closed-resource semantics + idempotent close ------------------------- */
{
    const m = new LinearRegression();
    assert(m.closed === false, "open initially");
    m.fit([[1], [2], [3]], [2, 4, 6]);
    m.close();
    assert(m.closed === true, "closed after close()");
    let threw = false;
    try { m.predict([[4]]); } catch { threw = true; }
    assert(threw, "use-after-close throws");
    m.close(); // idempotent, must not crash
    assert(m.closed === true, "still closed after 2nd close()");
}

/* --- reentrant-close attack: a valueOf that close()s during arg read ------ */
{
    // LinearRegression.fit reads X first; closing mid-read must be caught, no UAF
    const m = new LinearRegression();
    let threw = false;
    try {
        m.fit([[{ valueOf() { m.close(); return 1; } }]], [1]);
    } catch { threw = true; }
    assert(threw, "linreg.fit coerce-then-close is caught (no UAF)");
    m.close();
}
{
    const m = new LogisticRegression();
    m.fit([[0], [1], [5], [6]], [0, 0, 1, 1]);
    let threw = false;
    try {
        m.predict([[{ valueOf() { m.close(); return 1; } }]]);
    } catch { threw = true; }
    assert(threw, "logreg.predict coerce-then-close is caught (no UAF)");
    m.close();
}
{
    const m = new KMeans(2, 1);
    let threw = false;
    try {
        m.fit([[{ valueOf() { m.close(); return 1; } }], [2], [3]]);
    } catch { threw = true; }
    assert(threw, "kmeans.fit coerce-then-close is caught (no UAF)");
    m.close();
}

/* --- TypedArray ingest: flat Float64Array (X, y, rows, cols) -------------- */
{
    // recover y = 2x + 1 via a flat Float64Array with explicit shape
    const R = 20, C = 1;
    const Xf = new Float64Array(R * C), yf = new Float64Array(R);
    for (let x = 0; x < R; x++) { Xf[x] = x; yf[x] = 2 * x + 1; }
    const m = new LinearRegression();
    try {
        assert(m.fit(Xf, yf, R, C) === m, "flat fit returns this");
        // predict via a flat Float64Array (Xp, rows, cols)
        const Xp = new Float64Array([100, 0, -3]);
        const p = m.predict(Xp, 3, 1);
        approx(p[0], 201, 1e-4, "flat y(100)=201");
        approx(p[1], 1, 1e-4, "flat y(0)=1");
        approx(p[2], -5, 1e-4, "flat y(-3)=-5");
    } finally { m.close(); }
}

/* --- equivalence: flat Float64Array === Array-of-Array (exact doubles) ----- */
{
    const R = 500, C = 6;
    let s = 99;
    const rnd = () => { s = (s * 1103515245 + 12345) & 0x7fffffff; return s / 0x7fffffff * 2 - 1; };
    const w = []; for (let j = 0; j < C; j++) w.push(rnd());
    const Xaoa = [], yaoa = [], Xf = new Float64Array(R * C), yf = new Float64Array(R);
    for (let i = 0; i < R; i++) {
        const row = []; let t = 0.7;
        for (let j = 0; j < C; j++) { const v = rnd(); row.push(v); Xf[i * C + j] = v; t += v * w[j]; }
        Xaoa.push(row); yaoa.push(t); yf[i] = t;
    }
    // Array-of-Float64Array view sharing the flat buffer
    const Xaof = []; for (let i = 0; i < R; i++) Xaof.push(Xf.subarray(i * C, i * C + C));

    const mA = new LinearRegression(), mF = new LinearRegression(), mV = new LinearRegression();
    try {
        mA.fit(Xaoa, yaoa);
        mF.fit(Xf, yf, R, C);
        mV.fit(Xaof, yaoa);
        const q = [[1, 2, 3, 4, 5, 6], [-1, 0, 1, -2, 0, 2]];
        const qf = new Float64Array([1, 2, 3, 4, 5, 6, -1, 0, 1, -2, 0, 2]);
        const pA = mA.predict(q), pF = mF.predict(qf, 2, C), pV = mV.predict(Xaof.slice(0, 2).map((_, i) => Xaof[i]));
        for (let i = 0; i < 2; i++) {
            // same doubles + same scalar math => results are identical to 1e-9
            approx(pF[i], pA[i], 1e-9, "flat == AoA prediction " + i);
        }
        // Array-of-Float64Array fit matches AoA on the training coefficients too
        const pV2 = mV.predict(q);
        for (let i = 0; i < 2; i++)
            approx(pV2[i], pA[i], 1e-9, "AoF64 == AoA prediction " + i);
    } finally { mA.close(); mF.close(); mV.close(); }
}

/* --- flat-ingest error handling ------------------------------------------- */
{
    const m = new LinearRegression();
    try {
        const Xf = new Float64Array(6);
        let threw = false;
        try { m.fit(Xf, new Float64Array(3)); } catch { threw = true; } // no rows/cols
        assert(threw, "flat X without (rows,cols) throws");
        threw = false;
        try { m.fit(Xf, new Float64Array(3), 3, 3); } catch { threw = true; } // 3*3 != 6
        assert(threw, "flat length != rows*cols throws");
        threw = false;
        try { m.fit(Xf, new Float64Array(2), 3, 2); } catch { threw = true; } // y len 2 != 3
        assert(threw, "flat y length mismatch throws");
    } finally { m.close(); }
}

/* --- reentrant close during (rows,cols) coercion on the flat path --------- */
{
    // rows arg's valueOf closes the model mid-coercion; resolve-after-coerce
    // must catch it (throw), never a use-after-free.
    const m = new LinearRegression();
    const Xf = new Float64Array([1, 2, 3]);
    let threw = false;
    try {
        m.fit(Xf, new Float64Array([1, 2, 3]),
              { valueOf() { m.close(); return 3; } }, 1);
    } catch { threw = true; }
    assert(threw, "flat fit close-during-shape-coerce is caught (no UAF)");
    m.close();
}
{
    const m = new KMeans(2, 1);
    m.fit([[0], [1], [10], [11]], undefined); // AoA fit to make it fitted
    const Xf = new Float64Array([0, 10, 1]);
    let threw = false;
    try {
        m.predict(Xf, { valueOf() { m.close(); return 3; } }, 1);
    } catch { threw = true; }
    assert(threw, "kmeans flat predict close-during-coerce is caught (no UAF)");
    m.close();
}

/* --- Caps and adversarial-shape refusals (audit batch, 2026-08) ---------- */
{
    const X = [[1], [2], [3], [4]], y = [1, 2, 3, 4];
    const throwsRange = (fn, msg) => {
        n++;
        try { fn(); } catch (e) {
            if (!(e instanceof RangeError))
                throw new Error(msg + ": wrong error " + e.constructor.name);
            return;
        }
        throw new Error("expected a throw: " + msg);
    };
    throwsRange(() => new LogisticRegression({ maxIter: 100001 }),
                "logreg maxIter cap");
    throwsRange(() => new KMeans(80).fit(Array.from({ length: 100 }, (_, i) => [i])),
                "kmeans k near rows refusal");
    assert(new KMeans(50).fit(Array.from({ length: 100 }, (_, i) => [i])).inertia >= 0,
           "kmeans k <= rows/2 still fits");
    throwsRange(() => {
        const w = new LinearRegression();
        w.fit(X, y, { sampleWeight: [1e308, 1e308, 1e308, 1e308] });
    }, "sampleWeight sum overflow refusal");
    const km = new KMeans(2, { seed: 1 }); km.fit(X);
    throwsRange(() => km.predict([[NaN]]), "kmeans predict NaN refusal");
}

print("test_ml: all tests passed (" + n + " assertions)");
