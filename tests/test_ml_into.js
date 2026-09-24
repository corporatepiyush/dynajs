// flags: --std
/* test_ml_into.js — (predictInto / {as: "f64"}), (CSR on
 * predict/predictProba), ({nJobs, onProgress}) and (transformInto).
 *
 * Oracles:
 *   - predictInto / {as:"f64"} output is ELEMENT-IDENTICAL to predict()
 *     (Object.is, NaN positions included) across all 16 model variants
 *   - LinearRegression predictions are pinned against numpy.linalg.lstsq
 *     (generated with numpy 2.5.3, 17 significant digits)
 *   - CSR predictions equal the dense predictions of the same data
 *   - adversarial: short/wrong out buffers, malformed CSRs, nJobs validation
 *
 * Run: dynajs (CONFIG_NATIVE_MODULES=y) tests/test_ml_into.js */

import * as ml from "dyna:ml";

let n = 0;
function assert(c, m) { n++; if (!c) throw new Error("assert: " + m); }
function throws(fn, m) { let t = false; try { fn(); } catch { t = true; } assert(t, m); }
function eqArr(a, b) { if (a.length !== b.length) return false;
    for (let i = 0; i < a.length; i++) if (!Object.is(a[i], b[i])) return false; return true; }
function mulberry32(seed) { let a = seed >>> 0; return function () {
    a |= 0; a = (a + 0x6D2B79F5) | 0; let t = Math.imul(a ^ (a >>> 15), 1 | a);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t; return ((t ^ (t >>> 14)) >>> 0) / 4294967296; }; }

/* ----------------: predictInto == predict on every model ---------------- */
{
    const rnd = mulberry32(0xD4A1);
    const N = 40, F = 3;
    const X = [], y = [], yc = [];
    for (let i = 0; i < N; i++) {
        const row = [rnd() * 5, rnd() * 5 + 1, rnd() * 5 - 1];
        X.push(row);
        y.push(row[0] * 2 + row[1] - 1.5 + rnd() * 0.1);
        yc.push(row[0] + row[1] > 4 ? 1 : 0);
    }
    const Xq = X.slice(0, 10);

    /* every model with predict (plan enumeration: Linear, Logistic, KMeans,
     * trees, forests, GBR/GBC, XGB, SVC, GMM, NB, KN*), each regressor or
     * classifier as appropriate */
    const models = [
        ["LinearRegression", () => new ml.LinearRegression(), y],
        ["KMeans", () => new ml.KMeans(3, 7), null],
        ["SVC", () => new ml.SVC(), yc],
        ["GaussianMixture", () => new ml.GaussianMixture(3, { seed: 7, maxIter: 20 }), null],
        ["GaussianNB", () => new ml.GaussianNB(), yc],
        ["KNClassifier", () => new ml.KNClassifier(3), yc],
        ["KNRegressor", () => new ml.KNRegressor(3), y],
        ["DecisionTreeRegressor", () => new ml.DecisionTreeRegressor(), y],
        ["DecisionTreeClassifier", () => new ml.DecisionTreeClassifier(), yc],
        ["RandomForestRegressor", () => new ml.RandomForestRegressor({ nEstimators: 8, seed: 3 }), y],
        ["RandomForestClassifier", () => new ml.RandomForestClassifier({ nEstimators: 8, seed: 3 }), yc],
        ["GradientBoostingRegressor", () => new ml.GradientBoostingRegressor({ nEstimators: 8, seed: 3 }), y],
        ["GradientBoostingClassifier", () => new ml.GradientBoostingClassifier({ nEstimators: 8, seed: 3 }), yc],
        ["XGBRegressor", () => new ml.XGBRegressor({ nEstimators: 8, seed: 3 }), y],
        ["XGBClassifier", () => new ml.XGBClassifier({ nEstimators: 8, seed: 3 }), yc],
        ["LogisticRegression", () => new ml.LogisticRegression({ maxIter: 200 }), yc],
    ];
    for (const [name, make, target] of models) {
        const m = make();
        if (target) m.fit(X, target);
        else m.fit(X);
        const want = m.predict(Xq);
        const out = new Float64Array(want.length);
        const got = m.predictInto(out, Xq);
        assert(got === want.length, `${name}.predictInto returns the row count`);
        assert(eqArr([...out], [...want]), `${name}.predictInto == predict`);
        /* array-form X and flat-form X agree through the same into buffer */
        const flat = new Float64Array(Xq.length * F);
        for (let i = 0; i < Xq.length; i++)
            for (let j = 0; j < F; j++) flat[i * F + j] = Xq[i][j];
        const out2 = new Float64Array(want.length);
        m.predictInto(out2, flat, Xq.length, F);
        assert(eqArr([...out2], [...want]), `${name}.predictInto flat form`);
        /* extra capacity is left untouched */
        const out3 = new Float64Array(want.length + 3).fill(7);
        m.predictInto(out3, Xq);
        assert(eqArr([...out3.slice(0, want.length)], [...want]) &&
               eqArr([...out3.slice(want.length)], Array(3).fill(7)),
            `${name}.predictInto extra capacity untouched`);
    }

    /* {as: "f64"} returns a Float64Array, element-identical */
    for (const [name, make, target] of models) {
        const m = make();
        if (target) m.fit(X, target);
        else m.fit(X);
        const want = m.predict(Xq);
        const got = m.predict(Xq, { as: "f64" });
        assert(got instanceof Float64Array, `${name}.predict {as:"f64"} type`);
        assert(eqArr([...got], [...want]), `${name}.predict {as:"f64"} values`);
    }
    /* the bag is strict and the value is exact */
    const lr = new ml.LinearRegression(); lr.fit(X, y);
    throws(() => lr.predict(Xq, { as: "f32" }), "as: other value throws");
    throws(() => lr.predict(Xq, { as: "f64", extra: 1 }), "as bag strict");
    assert(lr.predict(Xq, { as: undefined }) instanceof Array, "as: undefined keeps Array");

    /* adversarial: short out, wrong type, and no-write-before-throw */
    {
        const out = new Float64Array(2).fill(7);
        throws(() => lr.predictInto(out, Xq), "short out throws");
        assert(eqArr([...out], [7, 7]), "short out: nothing written");
        throws(() => lr.predictInto(new Float64Array(3), [1, 2, 3]), "flat X without shape throws");
        throws(() => lr.predictInto([0, 0, 0], Xq), "non-Float64Array out throws");
        /* the RangeError names the sizes */
        let msg = "";
        try { lr.predictInto(new Float64Array(2), Xq); } catch (e) { msg = e.message; }
        assert(msg.includes("2") && msg.includes(String(Xq.length)),
            "short-out error names both sizes");
    }
    /* predictInto before fit refuses, as predict does */
    {
        const fresh = new ml.LinearRegression();
        throws(() => fresh.predictInto(new Float64Array(3), Xq), "predictInto before fit");
    }
}

/* numpy oracle: LinearRegression closed form vs lstsq on the same corpus
 * (python3 -c "import numpy as np; ...", numpy 2.5.3) */
{
    const X = [
        [0.0, 1.0], [1.0, 2.0], [2.0, 0.5], [3.0, 3.0], [4.0, 0.1],
        [5.0, 2.2], [6.0, 1.7], [7.0, 4.0], [8.0, 0.0], [9.0, 3.3],
    ];
    const y = [1.2, 3.9, 4.1, 8.8, 8.05, 13.3, 13.55, 18.9, 16.0, 21.15];
    /* numpy.linalg.lstsq(np.column_stack([X, np.ones(10)]), y, rcond=None)[0]
     * -> [2.0146282563813087, 1.1310865043341098, -0.1841611314306007] */
    const m = new ml.LinearRegression();
    m.fit(X, y);
    const out = new Float64Array(10);
    m.predictInto(out, X);
    const out2 = new Float64Array(2);
    m.predictInto(out2, [[1.5, 2.5], [4.5, 1.0]]);
    /* predictions for the two query rows, pinned the same way */
    const pin = [5.665497513976637, 10.012752526619398];
    const coef = m.coef, icept = m.intercept;
    assert(Math.abs(icept - (-0.1841611314306007)) < 1e-9, "intercept vs numpy lstsq");
    assert(Math.abs(coef[0] - 2.0146282563813087) < 1e-9, "coef[0] vs numpy lstsq");
    assert(Math.abs(coef[1] - 1.1310865043341098) < 1e-9, "coef[1] vs numpy lstsq");
    for (let i = 0; i < 2; i++)
        assert(Math.abs(out2[i] - pin[i]) < 1e-9, "predictInto row " + i + " vs numpy");
    assert(eqArr([...out.slice(0, 2)], [...m.predict(X).slice(0, 2)]),
        "into buffer matches predict on fit rows");
}

/* rev2: `out` is refused BY TYPE IDENTITY, not by element width -- a
 * BigInt64Array shares the 8-byte width and would silently reinterpret as
 * doubles. Every into-API refuses it. */
{
    const X = [[1, 2], [3, 4], [5, 6]];
    const yc = [0, 1, 0];
    const big = () => new BigInt64Array(6);
    let refused = 0;
    const models = [
        () => { const m = new ml.LinearRegression(); m.fit(X, [1, 2, 3]); return m; },
        () => { const m = new ml.LogisticRegression(); m.fit(X, yc); return m; },
        () => { const m = new ml.KMeans(2, 1); m.fit(X); return m; },
        () => { const m = new ml.SVC(); m.fit(X, yc); return m; },
        () => { const m = new ml.GaussianMixture(2, { seed: 1, maxIter: 5 }); m.fit(X); return m; },
        () => { const m = new ml.GaussianNB(); m.fit(X, yc); return m; },
        () => { const m = new ml.KNClassifier(1); m.fit(X, yc); return m; },
        () => { const m = new ml.KNRegressor(1); m.fit(X, [1, 2, 3]); return m; },
        () => { const m = new ml.DecisionTreeRegressor(); m.fit(X, [1, 2, 3]); return m; },
        () => { const m = new ml.DecisionTreeClassifier(); m.fit(X, yc); return m; },
        () => { const m = new ml.RandomForestRegressor({ nEstimators: 4, seed: 1 }); m.fit(X, [1, 2, 3]); return m; },
        () => { const m = new ml.RandomForestClassifier({ nEstimators: 4, seed: 1 }); m.fit(X, yc); return m; },
        () => { const m = new ml.GradientBoostingRegressor({ nEstimators: 4, seed: 1 }); m.fit(X, [1, 2, 3]); return m; },
        () => { const m = new ml.GradientBoostingClassifier({ nEstimators: 4, seed: 1 }); m.fit(X, yc); return m; },
        () => { const m = new ml.XGBRegressor({ nEstimators: 4, seed: 1 }); m.fit(X, [1, 2, 3]); return m; },
        () => { const m = new ml.XGBClassifier({ nEstimators: 4, seed: 1 }); m.fit(X, yc); return m; },
    ];
    for (const make of models) {
        const m = make();
        throws(() => m.predictInto(big(), X), "BigInt64Array out refused");
        refused++;
    }
    assert(refused === 16, "all 16 predict classes checked");
    /* the transformers refuse too */
    const sc = new ml.StandardScaler(); sc.fit(X);
    const mm = new ml.MinMaxScaler(); mm.fit(X);
    const pca = new ml.PCA({ nComponents: 1 }); pca.fit(X);
    throws(() => sc.transformInto(big(), X), "scaler BigInt out refused");
    throws(() => mm.transformInto(big(), X), "minmax BigInt out refused");
    throws(() => pca.transformInto(big(), X), "pca BigInt out refused");
    /* and the refusal is by identity: a real Float64Array still works */
    const out = new Float64Array(3);
    assert(models[0]().predictInto(out, X) === 3, "Float64Array out still accepted");
}

/* ---------------- CSR on predict / predictProba ---------------- */
{
    const X = [
        [1, 0, 0, 2.5],
        [0, 0, 3, 0],
        [0, 4, 0, 0],
        [2, 0, 0, 1],
    ];
    const yc = [0, 1, 1, 0];
    const yr = [3.5, 3, 4, 3];

    const lg = new ml.LogisticRegression({ maxIter: 500 });
    lg.fit(X, yc);
    const csr = ml.CSR.fromDense(X);
    assert(eqArr([...lg.predict(csr)], [...lg.predict(X)]), "logreg CSR predict == dense");
    const pd = lg.predictProba(X), ps = lg.predictProba(csr);
    assert(pd.length === ps.length, "logreg CSR proba shape");
    for (let i = 0; i < pd.length; i++)
        assert(eqArr(pd[i], ps[i]), `logreg CSR proba row ${i}`);
    /* into with a CSR X */
    {
        const out = new Float64Array(X.length);
        assert(lg.predictInto(out, csr) === X.length, "logreg predictInto(CSR)");
        assert(eqArr([...out], [...lg.predict(X)]), "logreg predictInto(CSR) values");
    }
    const lin = new ml.LinearRegression();
    lin.fit(X, yr);
    assert(eqArr([...lin.predict(csr)], [...lin.predict(X)]), "linreg CSR predict == dense");

    /* an explicitly-built CSR with UNSORTED columns inside a row */
    {
        const s = new ml.CSR([1, 1], [3, 0], [0, 2], 4);  /* cols 3 then 0 */
        const d = [[1, 0, 0, 1]];
        assert(eqArr([...lin.predict(s)], [...lin.predict(d)]),
            "unsorted column order inside a row");
    }
    /* duplicate indices within a row: CSR semantics SUM them (scipy's rule) */
    {
        const s = new ml.CSR([1, 1], [0, 0], [0, 2], 4);
        const d = [[2, 0, 0, 0]];
        assert(eqArr([...lin.predict(s)], [...lin.predict(d)]),
            "duplicate column indices sum");
    }
    /* the remaining estimators' predict/predictProba take a CSR too --
     * one sparse row materialised at a time into the same kernel the dense
     * form runs, so the two forms give the same answer. */
    {
        const rf = new ml.RandomForestRegressor({ nEstimators: 4, seed: 1 });
        rf.fit(X, yr);
        assert(eqArr([...rf.predict(csr)], [...rf.predict(X)]),
            "forest CSR predict == dense");
    }
    /* ...but their FIT still refuses a CSR, naming toDense */
    {
        const rf = new ml.RandomForestRegressor({ nEstimators: 4, seed: 1 });
        let msg = "";
        try { rf.fit(csr, yr); } catch (e) { msg = e.message; }
        assert(msg.includes("toDense"), "fit refusal names toDense()");
    }
}

/* rev2 bar: 30-sweep randomized CSR neutrality -- for 30 random sparsity
 * patterns, predict/predictProba on the CSR equals predict/predictProba on
 * the dense same data. This used to be a 1e-12 allclose, because the sparse
 * form walked its nonzeros (dyn_csr_dot) while the dense form ran dyn_ml_dot
 * and the two associated differently. The sparse form now expands each row
 * through the shared scratch and runs the SAME kernel, so the oracle is exact:
 * bit-exactness here is the sparse path. */
{
    const rnd = mulberry32(0xC57A);
    const lg = new ml.LogisticRegression({ maxIter: 300 });
    const lin = new ml.LinearRegression();
    const N = 24, F = 6;
    const X = [], yc = [], yr = [];
    for (let i = 0; i < N; i++) {
        const row = [];
        for (let j = 0; j < F; j++) row.push(rnd() * 4 - 2);
        X.push(row);
        yc.push(row[0] + row[1] > 0 ? 1 : 0);
        yr.push(row[0] * 2 - row[2] + 0.5);
    }
    lg.fit(X, yc);
    lin.fit(X, yr);
    for (let sweep = 0; sweep < 30; sweep++) {
        /* knock out a random ~60% of the cells: sparsity only, values kept */
        const sparse = X.map(row => row.map(v => (rnd() < 0.6 ? 0 : v)));
        const csr = ml.CSR.fromDense(sparse);
        assert(eqArr([...lin.predict(csr)], [...lin.predict(sparse)]),
            `sweep ${sweep}: linreg CSR == dense`);
        assert(eqArr([...lg.predict(csr)], [...lg.predict(sparse)]),
            `sweep ${sweep}: logreg CSR == dense`);
        const pd = lg.predictProba(sparse), ps = lg.predictProba(csr);
        for (let i = 0; i < N; i++)
            assert(eqArr(pd[i], ps[i]), `sweep ${sweep}: proba row ${i}`);
        const out = new Float64Array(N);
        lin.predictInto(out, csr);
        assert(eqArr([...out], [...lin.predict(sparse)]),
            `sweep ${sweep}: predictInto(CSR) == dense`);
    }
}

/* ----------------: transformInto ---------------- */
{
    const X = [[0, 10], [1, 11], [2, 12], [3, 13]];
    const sc = new ml.StandardScaler();
    sc.fit(X);
    const want = sc.transform(X);
    const out = new Float64Array(X.length * 2);
    assert(sc.transformInto(out, X) === X.length, "scaler transformInto row count");
    assert(eqArr([...out], [...want.flat()]), "scaler transformInto == transform");
    /* flat form */
    const flat = new Float64Array([0, 10, 1, 11, 2, 12, 3, 13]);
    const out2 = new Float64Array(8);
    sc.transformInto(out2, flat, 4, 2);
    assert(eqArr([...out2], [...want.flat()]), "scaler transformInto flat form");
    /* MinMaxScaler */
    const mm = new ml.MinMaxScaler();
    mm.fit(X);
    const w2 = mm.transform(X);
    const out3 = new Float64Array(8);
    mm.transformInto(out3, X);
    assert(eqArr([...out3], [...w2.flat()]), "minmax transformInto == transform");
    /* PCA */
    const pca = new ml.PCA({ nComponents: 2 });
    pca.fit(X);
    const w3 = pca.transform(X);
    const out4 = new Float64Array(X.length * 2);
    assert(pca.transformInto(out4, X) === X.length, "pca transformInto row count");
    assert(eqArr([...out4], [...w3.flat()]), "pca transformInto == transform");
    /* adversarial */
    throws(() => sc.transformInto(new Float64Array(2), X), "transformInto short out");
    throws(() => sc.transformInto([0, 0], X), "transformInto wrong type");
    const fresh = new ml.StandardScaler();
    throws(() => fresh.transformInto(new Float64Array(8), X), "transformInto before fit");
}

/* ----------------: {nJobs, onProgress} ---------------- */
{
    const X = [], y = [];
    for (let i = 0; i < 20; i++) { X.push([i]); y.push(i); }
    const events = [];
    const scores = ml.crossValScore(() => new ml.LinearRegression(), X, y,
        { k: 4, nJobs: 4, onProgress: (p) => events.push({ ...p }) });
    assert(scores.length === 4, "crossValScore folds");
    assert(events.length === 4, "one progress event per fold");
    assert(eqArr(events.map(e => e.done), [1, 2, 3, 4]), "progress done sequence");
    assert(events.every(e => e.total === 4), "progress total");
    /* nJobs=1 gives identical scores to nJobs=4 (the sequential fast path) */
    const scores1 = ml.crossValScore(() => new ml.LinearRegression(), X, y, { k: 4 });
    assert(eqArr(scores, scores1), "nJobs is sequential: identical results");

    /* gridSearch: progress counts folds across the WHOLE search */
    {
        let seen = [];
        const g = ml.gridSearch(() => new ml.LinearRegression(), X.slice(0, 8), y.slice(0, 8),
            { fitIntercept: [true, false] },
            { k: 2, nJobs: 2, onProgress: (p) => seen.push(p) });
        assert(g.results.length === 2, "grid combos");
        assert(seen.length === 4, "one event per (combo, fold)");
        assert(seen[seen.length - 1].total === 4, "grid progress total = combos*folds");
        /* nJobs <= 0 is refused BEFORE any work */
        throws(() => ml.crossValScore(() => new ml.LinearRegression(), X, y,
            { k: 2, nJobs: 0 }), "crossValScore nJobs=0");
        throws(() => ml.gridSearch(() => new ml.LinearRegression(), X, y,
            { a: [1] }, { k: 2, nJobs: -3 }), "gridSearch nJobs<0");
        throws(() => ml.randomSearch(() => new ml.LinearRegression(), X, y,
            { a: [1] }, { k: 2, nJobs: 0 }), "randomSearch nJobs=0");
        throws(() => ml.crossValScore(() => new ml.LinearRegression(), X, y,
            { k: 2, onProgress: 42 }), "onProgress must be a function");
    }
}

console.log(`test_ml_into: ${n} assertions ok`);
