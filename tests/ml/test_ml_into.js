// flags: --std
import * as ml from "dyna:ml";

let n = 0;
function assert(c, m) { n++; if (!c) throw new Error("assert: " + m); }
async function throws(fn, m) { let t = false; try { await fn(); } catch { t = true; } assert(t, m); }
function eqArr(a, b) { if (a.length !== b.length) return false;
    for (let i = 0; i < a.length; i++) if (!Object.is(a[i], b[i])) return false; return true; }
function mulberry32(seed) { let a = seed >>> 0; return function () {
    a |= 0; a = (a + 0x6D2B79F5) | 0; let t = Math.imul(a ^ (a >>> 15), 1 | a);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t; return ((t ^ (t >>> 14)) >>> 0) / 4294967296; }; }

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
        const m = await make();
        if (target) await m.fit(X, target);
        else await m.fit(X);
        const want = await m.predict(Xq);
        const out = new Float64Array(want.length);
        const got = await m.predictInto(out, Xq);
        assert(got === want.length, `${name}.predictInto returns the row count`);
        assert(eqArr([...out], [...want]), `${name}.predictInto == predict`);
        const flat = new Float64Array(Xq.length * F);
        for (let i = 0; i < Xq.length; i++)
            for (let j = 0; j < F; j++) flat[i * F + j] = Xq[i][j];
        const out2 = new Float64Array(want.length);
        await m.predictInto(out2, flat, Xq.length, F);
        assert(eqArr([...out2], [...want]), `${name}.predictInto flat form`);
        const out3 = new Float64Array(want.length + 3).fill(7);
        await m.predictInto(out3, Xq);
        assert(eqArr([...out3.slice(0, want.length)], [...want]) &&
               eqArr([...out3.slice(want.length)], Array(3).fill(7)),
            `${name}.predictInto extra capacity untouched`);
    }

    for (const [name, make, target] of models) {
        const m = await make();
        if (target) await m.fit(X, target);
        else await m.fit(X);
        const want = await m.predict(Xq);
        const got = await m.predict(Xq, { as: "f64" });
        assert(got instanceof Float64Array, `${name}.predict {as:"f64"} type`);
        assert(eqArr([...got], [...want]), `${name}.predict {as:"f64"} values`);
    }
    const lr = new ml.LinearRegression(); await lr.fit(X, y);
    await throws(async () => await lr.predict(Xq, { as: "f32" }), "as: other value throws");
    await throws(async () => await lr.predict(Xq, { as: "f64", extra: 1 }), "as bag strict");
    assert((await lr.predict(Xq, { as: undefined })) instanceof Array, "as: undefined keeps Array");

    {
        const out = new Float64Array(2).fill(7);
        await throws(async () => await lr.predictInto(out, Xq), "short out throws");
        assert(eqArr([...out], [7, 7]), "short out: nothing written");
        await throws(async () => await lr.predictInto(new Float64Array(3), [1, 2, 3]), "flat X without shape throws");
        await throws(async () => await lr.predictInto([0, 0, 0], Xq), "non-Float64Array out throws");
        let msg = "";
        try { await lr.predictInto(new Float64Array(2), Xq); } catch (e) { msg = e.message; }
        assert(msg.includes("2") && msg.includes(String(Xq.length)),
            "short-out error names both sizes");
    }
    {
        const fresh = new ml.LinearRegression();
        await throws(async () => await fresh.predictInto(new Float64Array(3), Xq), "predictInto before fit");
    }
}

{
    const X = [
        [0.0, 1.0], [1.0, 2.0], [2.0, 0.5], [3.0, 3.0], [4.0, 0.1],
        [5.0, 2.2], [6.0, 1.7], [7.0, 4.0], [8.0, 0.0], [9.0, 3.3],
    ];
    const y = [1.2, 3.9, 4.1, 8.8, 8.05, 13.3, 13.55, 18.9, 16.0, 21.15];
    const m = new ml.LinearRegression();
    await m.fit(X, y);
    const out = new Float64Array(10);
    await m.predictInto(out, X);
    const out2 = new Float64Array(2);
    await m.predictInto(out2, [[1.5, 2.5], [4.5, 1.0]]);
    const pin = [5.665497513976637, 10.012752526619398];
    const coef = m.coef, icept = m.intercept;
    assert(Math.abs(icept - (-0.1841611314306007)) < 1e-9, "intercept vs numpy lstsq");
    assert(Math.abs(coef[0] - 2.0146282563813087) < 1e-9, "coef[0] vs numpy lstsq");
    assert(Math.abs(coef[1] - 1.1310865043341098) < 1e-9, "coef[1] vs numpy lstsq");
    for (let i = 0; i < 2; i++)
        assert(Math.abs(out2[i] - pin[i]) < 1e-9, "predictInto row " + i + " vs numpy");
    assert(eqArr([...out.slice(0, 2)], [...(await m.predict(X)).slice(0, 2)]),
        "into buffer matches predict on fit rows");
}

{
    const X = [[1, 2], [3, 4], [5, 6]];
    const yc = [0, 1, 0];
    const big = () => new BigInt64Array(6);
    let refused = 0;
    const models = [
        async () => { const m = new ml.LinearRegression(); await m.fit(X, [1, 2, 3]); return m; },
        async () => { const m = new ml.LogisticRegression(); await m.fit(X, yc); return m; },
        async () => { const m = new ml.KMeans(2, 1); await m.fit(X); return m; },
        async () => { const m = new ml.SVC(); await m.fit(X, yc); return m; },
        async () => { const m = new ml.GaussianMixture(2, { seed: 1, maxIter: 5 }); await m.fit(X); return m; },
        async () => { const m = new ml.GaussianNB(); await m.fit(X, yc); return m; },
        async () => { const m = new ml.KNClassifier(1); await m.fit(X, yc); return m; },
        async () => { const m = new ml.KNRegressor(1); await m.fit(X, [1, 2, 3]); return m; },
        async () => { const m = new ml.DecisionTreeRegressor(); await m.fit(X, [1, 2, 3]); return m; },
        async () => { const m = new ml.DecisionTreeClassifier(); await m.fit(X, yc); return m; },
        async () => { const m = new ml.RandomForestRegressor({ nEstimators: 4, seed: 1 }); await m.fit(X, [1, 2, 3]); return m; },
        async () => { const m = new ml.RandomForestClassifier({ nEstimators: 4, seed: 1 }); await m.fit(X, yc); return m; },
        async () => { const m = new ml.GradientBoostingRegressor({ nEstimators: 4, seed: 1 }); await m.fit(X, [1, 2, 3]); return m; },
        async () => { const m = new ml.GradientBoostingClassifier({ nEstimators: 4, seed: 1 }); await m.fit(X, yc); return m; },
        async () => { const m = new ml.XGBRegressor({ nEstimators: 4, seed: 1 }); await m.fit(X, [1, 2, 3]); return m; },
        async () => { const m = new ml.XGBClassifier({ nEstimators: 4, seed: 1 }); await m.fit(X, yc); return m; },
    ];
    for (const make of models) {
        const m = await make();
        await throws(async () => await m.predictInto(big(), X), "BigInt64Array out refused");
        refused++;
    }
    assert(refused === 16, "all 16 predict classes checked");
    const sc = new ml.StandardScaler(); await sc.fit(X);
    const mm = new ml.MinMaxScaler(); await mm.fit(X);
    const pca = new ml.PCA({ nComponents: 1 }); await pca.fit(X);
    await throws(async () => await sc.transformInto(big(), X), "scaler BigInt out refused");
    await throws(async () => await mm.transformInto(big(), X), "minmax BigInt out refused");
    await throws(async () => await pca.transformInto(big(), X), "pca BigInt out refused");
    const out = new Float64Array(3);
    assert((await (await models[0]()).predictInto(out, X)) === 3, "Float64Array out still accepted");
}

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
    await lg.fit(X, yc);
    const csr = await ml.CSR.fromDense(X);
    assert(eqArr([...await lg.predict(csr)], [...await lg.predict(X)]), "logreg CSR predict == dense");
    const pd = await lg.predictProba(X), ps = await lg.predictProba(csr);
    assert(pd.length === ps.length, "logreg CSR proba shape");
    for (let i = 0; i < pd.length; i++)
        assert(eqArr(pd[i], ps[i]), `logreg CSR proba row ${i}`);
    {
        const out = new Float64Array(X.length);
        assert((await lg.predictInto(out, csr)) === X.length, "logreg predictInto(CSR)");
        assert(eqArr([...out], [...await lg.predict(X)]), "logreg predictInto(CSR) values");
    }
    const lin = new ml.LinearRegression();
    await lin.fit(X, yr);
    assert(eqArr([...await lin.predict(csr)], [...await lin.predict(X)]), "linreg CSR predict == dense");

    {
        const s = new ml.CSR([1, 1], [3, 0], [0, 2], 4);
        const d = [[1, 0, 0, 1]];
        assert(eqArr([...await lin.predict(s)], [...await lin.predict(d)]),
            "unsorted column order inside a row");
    }
    {
        const s = new ml.CSR([1, 1], [0, 0], [0, 2], 4);
        const d = [[2, 0, 0, 0]];
        assert(eqArr([...await lin.predict(s)], [...await lin.predict(d)]),
            "duplicate column indices sum");
    }
    {
        const rf = new ml.RandomForestRegressor({ nEstimators: 4, seed: 1 });
        await rf.fit(X, yr);
        assert(eqArr([...await rf.predict(csr)], [...await rf.predict(X)]),
            "forest CSR predict == dense");
    }
    {
        const rf = new ml.RandomForestRegressor({ nEstimators: 4, seed: 1 });
        let msg = "";
        try { await rf.fit(csr, yr); } catch (e) { msg = e.message; }
        assert(msg.includes("toDense"), "fit refusal names toDense()");
    }
}

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
    await lg.fit(X, yc);
    await lin.fit(X, yr);
    for (let sweep = 0; sweep < 30; sweep++) {
        const sparse = X.map(row => row.map(v => (rnd() < 0.6 ? 0 : v)));
        const csr = await ml.CSR.fromDense(sparse);
        assert(eqArr([...await lin.predict(csr)], [...await lin.predict(sparse)]),
            `sweep ${sweep}: linreg CSR == dense`);
        assert(eqArr([...await lg.predict(csr)], [...await lg.predict(sparse)]),
            `sweep ${sweep}: logreg CSR == dense`);
        const pd = await lg.predictProba(sparse), ps = await lg.predictProba(csr);
        for (let i = 0; i < N; i++)
            assert(eqArr(pd[i], ps[i]), `sweep ${sweep}: proba row ${i}`);
        const out = new Float64Array(N);
        await lin.predictInto(out, csr);
        assert(eqArr([...out], [...await lin.predict(sparse)]),
            `sweep ${sweep}: predictInto(CSR) == dense`);
    }
}

{
    const X = [[0, 10], [1, 11], [2, 12], [3, 13]];
    const sc = new ml.StandardScaler();
    await sc.fit(X);
    const want = await sc.transform(X);
    const out = new Float64Array(X.length * 2);
    assert((await sc.transformInto(out, X)) === X.length, "scaler transformInto row count");
    assert(eqArr([...out], [...want.flat()]), "scaler transformInto == transform");
    const flat = new Float64Array([0, 10, 1, 11, 2, 12, 3, 13]);
    const out2 = new Float64Array(8);
    await sc.transformInto(out2, flat, 4, 2);
    assert(eqArr([...out2], [...want.flat()]), "scaler transformInto flat form");
    const mm = new ml.MinMaxScaler();
    await mm.fit(X);
    const w2 = await mm.transform(X);
    const out3 = new Float64Array(8);
    await mm.transformInto(out3, X);
    assert(eqArr([...out3], [...w2.flat()]), "minmax transformInto == transform");
    const pca = new ml.PCA({ nComponents: 2 });
    await pca.fit(X);
    const w3 = await pca.transform(X);
    const out4 = new Float64Array(X.length * 2);
    assert((await pca.transformInto(out4, X)) === X.length, "pca transformInto row count");
    assert(eqArr([...out4], [...w3.flat()]), "pca transformInto == transform");
    await throws(async () => await sc.transformInto(new Float64Array(2), X), "transformInto short out");
    await throws(async () => await sc.transformInto([0, 0], X), "transformInto wrong type");
    const fresh = new ml.StandardScaler();
    await throws(async () => await fresh.transformInto(new Float64Array(8), X), "transformInto before fit");
}

{
    const X = [], y = [];
    for (let i = 0; i < 20; i++) { X.push([i]); y.push(i); }
    const events = [];
    const scores = await ml.crossValScore(() => new ml.LinearRegression(), X, y,
        { k: 4, nJobs: 4, onProgress: (p) => events.push({ ...p }) });
    assert(scores.length === 4, "crossValScore folds");
    assert(events.length === 4, "one progress event per fold");
    assert(eqArr(events.map(e => e.done), [1, 2, 3, 4]), "progress done sequence");
    assert(events.every(e => e.total === 4), "progress total");
    const scores1 = await ml.crossValScore(() => new ml.LinearRegression(), X, y, { k: 4 });
    assert(eqArr(scores, scores1), "nJobs is sequential: identical results");

    {
        let seen = [];
        const g = await ml.gridSearch(() => new ml.LinearRegression(), X.slice(0, 8), y.slice(0, 8),
            { fitIntercept: [true, false] },
            { k: 2, nJobs: 2, onProgress: (p) => seen.push(p) });
        assert(g.results.length === 2, "grid combos");
        assert(seen.length === 4, "one event per (combo, fold)");
        assert(seen[seen.length - 1].total === 4, "grid progress total = combos*folds");
        await throws(async () => await ml.crossValScore(() => new ml.LinearRegression(), X, y,
            { k: 2, nJobs: 0 }), "crossValScore nJobs=0");
        await throws(async () => await ml.gridSearch(() => new ml.LinearRegression(), X, y,
            { a: [1] }, { k: 2, nJobs: -3 }), "gridSearch nJobs<0");
        await throws(async () => await ml.randomSearch(() => new ml.LinearRegression(), X, y,
            { a: [1] }, { k: 2, nJobs: 0 }), "randomSearch nJobs=0");
        await throws(async () => await ml.crossValScore(() => new ml.LinearRegression(), X, y,
            { k: 2, onProgress: 42 }), "onProgress must be a function");
    }
}

console.log(`test_ml_into: ${n} assertions ok`);
