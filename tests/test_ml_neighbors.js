/* test_ml_neighbors.js — dyna:ml KNClassifier / KNRegressor / DBScan.
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_ml_neighbors.js
 * Prints "test_ml_neighbors: all tests passed" on success; throws on failure. */

import { KNClassifier, KNRegressor, DBScan } from "dyna:ml";

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
/* Reference k-NN in JS: the oracle every native prediction below is checked
 * against. Deliberately written the slow, obvious way. */
function refKnn(Xtrain, ytrain, k, weighted, regressor, q) {
    const d = Xtrain.map((row, i) => {
        let s = 0;
        for (let j = 0; j < row.length; j++) { const t = row[j] - q[j]; s += t * t; }
        return { d: s, y: ytrain[i], i };
    });
    /* ties in distance broken by index, matching a stable scan */
    d.sort((a, b) => (a.d - b.d) || (a.i - b.i));
    const near = d.slice(0, k);
    if (regressor) {
        if (weighted) {
            const hit = near.find((e) => e.d === 0);
            if (hit) return hit.y;
            let num = 0, den = 0;
            for (const e of near) { const w = 1 / Math.sqrt(e.d); num += w * e.y; den += w; }
            return num / den;
        }
        return near.reduce((s, e) => s + e.y, 0) / k;
    }
    const score = new Map();
    for (const e of near) {
        const w = weighted ? (e.d === 0 ? Infinity : 1 / Math.sqrt(e.d)) : 1;
        score.set(e.y, (score.get(e.y) || 0) + w);
    }
    let bestLab = null, bestScore = -1;
    for (const [lab, sc] of score)
        if (sc > bestScore || (sc === bestScore && lab < bestLab)) { bestScore = sc; bestLab = lab; }
    return bestLab;
}

/* ===================== KNClassifier ===================== */
{
    /* two well-separated blobs, k=3 */
    const X = [[0, 0], [0, 1], [1, 0], [10, 10], [10, 11], [11, 10]];
    const y = [0, 0, 0, 1, 1, 1];
    const m = new KNClassifier(3);
    try {
        assert(m.fit(X, y) === m, "fit returns this");
        const p = m.predict([[0.5, 0.5], [10.5, 10.5], [0, 0]]);
        assert(Array.isArray(p) && p.length === 3, "predict returns an Array of 3");
        assert(p[0] === 0, "near blob A -> 0");
        assert(p[1] === 1, "near blob B -> 1");
        assert(p[2] === 0, "an exact training point -> its own label");
        /* every training point is its own nearest neighbour, so k=1 reproduces y */
        const m1 = new KNClassifier(1);
        try {
            m1.fit(X, y);
            const self = m1.predict(X);
            for (let i = 0; i < X.length; i++)
                assert(self[i] === y[i], `k=1 reproduces training label ${i}`);
        } finally { m1.close(); }
    } finally { m.close(); }
}
{
    /* differential: native vs the JS reference over many random queries */
    function rng(seed) {
        let s = seed >>> 0;
        return () => { s = (s * 1664525 + 1013904223) >>> 0; return s / 4294967296; };
    }
    let cases = 0;
    for (const cols of [1, 3, 8, 17]) {
        for (const k of [1, 3, 5]) {
            for (const weighted of [false, true]) {
                const rnd = rng(9000 + cols * 31 + k * 7 + (weighted ? 1 : 0));
                const rows = 40;
                const X = [], y = [];
                for (let i = 0; i < rows; i++) {
                    const row = [];
                    for (let j = 0; j < cols; j++) row.push(Math.round(rnd() * 20) / 2);
                    X.push(row);
                    y.push(Math.floor(rnd() * 3));      /* 3 classes */
                }
                const clf = new KNClassifier(k, weighted ? "distance" : "uniform");
                const reg = new KNRegressor(k, weighted ? "distance" : "uniform");
                try {
                    clf.fit(X, y);
                    reg.fit(X, y);
                    const Q = [];
                    for (let t = 0; t < 25; t++) {
                        const row = [];
                        for (let j = 0; j < cols; j++) row.push(Math.round(rnd() * 20) / 2);
                        Q.push(row);
                    }
                    Q.push(X[0]);                        /* an exact hit: distance 0 */
                    const gotC = clf.predict(Q), gotR = reg.predict(Q);
                    for (let t = 0; t < Q.length; t++) {
                        cases++;
                        const wantC = refKnn(X, y, k, weighted, false, Q[t]);
                        const wantR = refKnn(X, y, k, weighted, true, Q[t]);
                        assert(gotC[t] === wantC,
                            `clf cols=${cols} k=${k} w=${weighted} q=${t}: ${gotC[t]} vs ${wantC}`);
                        approx(gotR[t], wantR, 1e-9,
                            `reg cols=${cols} k=${k} w=${weighted} q=${t}`);
                    }
                } finally { clf.close(); reg.close(); }
            }
        }
    }
    assert(cases === 24 * 26, "expected 624 differential cases, got " + cases);
}

/* ===================== KNRegressor ===================== */
{
    /* uniform mean of the k nearest targets */
    const X = [[0], [1], [2], [3]];
    const y = [10, 20, 30, 40];
    const m = new KNRegressor(2);
    try {
        m.fit(X, y);
        approx(m.predict([[0.5]])[0], 15, 1e-12, "mean of the two nearest");
        approx(m.predict([[2.4]])[0], 35, 1e-12, "mean of 30 and 40");
    } finally { m.close(); }
}
{
    /* distance weighting: an exact training point returns exactly its target,
     * not a blend, because a zero distance is an infinite weight */
    const m = new KNRegressor(3, "distance");
    try {
        m.fit([[0], [1], [2]], [5, 100, 200]);
        approx(m.predict([[0]])[0], 5, 0, "exact hit returns its own target exactly");
        const mid = m.predict([[0.5]])[0];
        assert(mid > 5 && mid < 200, "a between-point blends its neighbours");
    } finally { m.close(); }
}

/* ===================== KN errors ===================== */
{
    throws(() => new KNClassifier(0), "k must be positive", RangeError);
    throws(() => new KNClassifier(-1), "negative k", RangeError);
    throws(() => new KNClassifier(3, "nearest"), "bad weights string", TypeError);
    const m = new KNClassifier(5);
    try {
        throws(() => m.fit([[1], [2]], [0, 1]), "fewer rows than k", RangeError);
        throws(() => m.predict([[1]]), "predict before fit");
        m.fit([[1], [2], [3], [4], [5]], [0, 0, 1, 1, 1]);
        throws(() => m.predict([[1, 2]]), "feature count mismatch", TypeError);
        throws(() => m.fit([[1], [2], [3], [4], [5]], [0, 1]), "y length mismatch", TypeError);
    } finally { m.close(); }
}

/* ===================== DBScan ===================== */
{
    /* two dense blobs plus one far-away outlier */
    const X = [
        [0, 0], [0, 0.2], [0.2, 0], [0.1, 0.1],       /* blob A */
        [5, 5], [5, 5.2], [5.2, 5], [5.1, 5.1],       /* blob B */
        [50, 50],                                     /* noise */
    ];
    const m = new DBScan(1.0, 3);
    try {
        assert(m.fit(X) === m, "fit returns this");
        assert(m.nClusters === 2, "two clusters found, got " + m.nClusters);
        approx(m.eps, 1.0, 0, "eps getter");
        const L = m.labels;
        assert(L.length === 9, "one label per point");
        assert(L[8] === -1, "the far point is noise (-1)");
        for (let i = 1; i < 4; i++) assert(L[i] === L[0], "blob A is one cluster @" + i);
        for (let i = 5; i < 8; i++) assert(L[i] === L[4], "blob B is one cluster @" + i);
        assert(L[0] !== L[4], "the blobs are different clusters");
        assert(L[0] === 0 && L[4] === 1, "cluster ids are assigned in scan order");
    } finally { m.close(); }
}
{
    /* every point isolated relative to eps -> all noise, no clusters */
    const m = new DBScan(0.1, 2);
    try {
        m.fit([[0, 0], [10, 10], [20, 20]]);
        assert(m.nClusters === 0, "no clusters");
        assert(m.labels.every((l) => l === -1), "every point is noise");
    } finally { m.close(); }
}
{
    /* minPts = 1 makes every point a core point, so nothing is ever noise */
    const m = new DBScan(0.1, 1);
    try {
        m.fit([[0, 0], [10, 10], [20, 20]]);
        assert(m.nClusters === 3, "each point its own cluster");
        assert(m.labels.every((l) => l >= 0), "nothing is noise when minPts is 1");
    } finally { m.close(); }
}
{
    /* a chain: transitivity must merge it into ONE cluster even though the
     * endpoints are far apart -- the property that distinguishes DBScan from
     * a distance-to-centre method */
    const X = [];
    for (let i = 0; i < 20; i++) X.push([i * 0.5, 0]);
    const m = new DBScan(0.6, 2);
    try {
        m.fit(X);
        assert(m.nClusters === 1, "the chain is a single cluster, got " + m.nClusters);
        assert(m.labels.every((l) => l === 0), "every link in the chain is labelled 0");
        /* endpoints are 9.5 apart, far beyond eps */
        approx(X[19][0] - X[0][0], 9.5, 0, "the chain really is longer than eps");
    } finally { m.close(); }
}
{
    /* labels/nClusters before fit are empty/0, not an error */
    const m = new DBScan();
    try {
        assert(m.labels.length === 0, "no labels before fit");
        assert(m.nClusters === 0, "no clusters before fit");
        approx(m.eps, 0.5, 0, "default eps");
    } finally { m.close(); }
}
{
    /* the flat Float64Array form agrees with the Array form */
    const rowsArr = [[0, 0], [0, 0.1], [5, 5], [5, 5.1]];
    const flat = new Float64Array([0, 0, 0, 0.1, 5, 5, 5, 5.1]);
    const a = new DBScan(1, 2), b = new DBScan(1, 2);
    try {
        a.fit(rowsArr);
        b.fit(flat, 4, 2);
        assert(a.nClusters === b.nClusters, "same cluster count");
        for (let i = 0; i < 4; i++)
            assert(a.labels[i] === b.labels[i], "same label at " + i);
    } finally { a.close(); b.close(); }
}
{
    throws(() => new DBScan(0), "eps must be positive", RangeError);
    throws(() => new DBScan(-1), "negative eps", RangeError);
    throws(() => new DBScan(1, 0), "minPts at least 1", RangeError);
}

/* ===================== reentrancy: valueOf closing the model ===================== */
{
    const m = new KNClassifier(1);
    m.fit([[1], [2]], [0, 1]);
    throws(() => m.predict([[{ valueOf() { m.close(); return 1; } }]]),
        "reentrant close during predict");
    assert(m.closed === true, "closed by the hostile valueOf");
}
{
    const m = new KNRegressor(1);
    throws(() => m.fit([[{ valueOf() { m.close(); return 1; } }], [2]], [0, 1]),
        "reentrant close during fit");
    assert(m.closed === true, "closed");
}
{
    const m = new DBScan(1, 1);
    throws(() => m.fit([[{ valueOf() { m.close(); return 1; } }]]),
        "reentrant close during DBScan fit");
    assert(m.closed === true, "closed");
}

/* --- NaN predict refusal (audit batch, 2026-08) --------------------------- */
{
    const X = [[0], [1], [2], [3]], y = [0, 0, 1, 1];
    const m = new KNClassifier(3); m.fit(X, y);
    const throwsRange = (fn, msg) => {
        n++;
        try { fn(); } catch (e) {
            if (!(e instanceof RangeError))
                throw new Error(msg + ": wrong error " + e.constructor.name);
            return;
        }
        throw new Error("expected a throw: " + msg);
    };
    throwsRange(() => m.predict([[NaN]]), "knn predict NaN refusal");
}

print("test_ml_neighbors: all tests passed (" + n + " assertions)");
