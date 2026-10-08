import {
    averagePrecision, stratifiedKFold, kFold, confusionMatrix, rocAuc,
    crossValScore, gridSearch,
    LinearRegression, LogisticRegression, KMeans, SVC, GaussianMixture,
    GaussianNB, DecisionTreeClassifier, DecisionTreeRegressor,
    RandomForestClassifier, RandomForestRegressor, GradientBoostingClassifier,
    GradientBoostingRegressor, XGBClassifier, XGBRegressor, PCA, KNClassifier,
    KNRegressor, DBScan, StandardScaler, MinMaxScaler, CSR, Pipeline,
} from "dyna:ml";

let n = 0;
function assert(c, m) { n++; if (!c) throw new Error("assertion failed: " + m); }
function close(got, want, m, tol = 1e-12) {
    n++;
    if (!(Math.abs(got - want) <= tol))
        throw new Error("assertion failed: " + m + "\n  got:  " + got + "\n  want: " + want);
}
function throwsWhich(fn, ctor, m) {
    n++;
    try { fn(); } catch (e) {
        if (e instanceof ctor) return;
        throw new Error("assertion failed (wrong error): " + m + " -> " + e);
    }
    throw new Error("assertion failed (expected " + ctor.name + "): " + m);
}
const throwsRange = (fn, m) => throwsWhich(fn, RangeError, m);
const throwsAny = (fn, m) => throwsWhich(fn, Error, m);
function lcg(seed) {
    let s = seed >>> 0;
    return () => { s = (s * 1664525 + 1013904223) >>> 0; return s / 4294967296; };
}
function dataset(seed, rows, cols) {
    const r = lcg(seed), X = [], y = [];
    for (let i = 0; i < rows; i++) {
        const row = [];
        for (let j = 0; j < cols; j++) row.push(r() * 4 - 2);
        X.push(row);
        y.push(row[0] + row[1] > 0 ? 1 : 0);
    }
    return { X, y };
}

{
    close(averagePrecision([1, 0, 1], [0.9, 0.9, 0.1]), 7 / 12,
          "AP tie group is one threshold");
    close(averagePrecision([1, 1, 0], [0.5, 0.5, 0.5]), 2 / 3,
          "AP all-tied = base rate");
    close(averagePrecision([1, 1, 0, 0], [0.9, 0.9, 0.1, 0.1]), 1.0,
          "AP pure tie runs");
    close(averagePrecision([0, 0, 1, 1], [0.1, 0.2, 0.8, 0.9]), 1.0,
          "AP separable unchanged");
    close(averagePrecision([1, 0, 0], [0.9, 0.5, 0.5]), 1.0,
          "AP negative-only tie group adds nothing");
}

{
    throwsRange(() => stratifiedKFold([7, 0, 0, 0, 0, 0], { k: 3 }),
                "lone member of class 7, k=3 refused");
    throwsRange(() => stratifiedKFold([1, 1, 2, 0, 0, 0, 0, 0], { k: 4 }),
                "class with 2 members, k=4 refused");
    n++;
    stratifiedKFold([1, 1, 1, 0, 0, 0], { k: 3 });
    throwsRange(() => stratifiedKFold([7, 0, 0, 0, 0, 0], { k: 3, shuffle: true, seed: 1 }),
                "shuffled lone member refused");
}

{
    for (const bad of [NaN, Infinity]) {
        throwsAny(() => new CSR([bad, 2], [0, 1], [0, 2], 3),
                  "ctor refuses " + String(bad) + " carried into a CSR");
        throwsAny(() => CSR.fromDense([[1, 2], [bad, 3]]),
                  "fromDense refuses " + String(bad) + " carried into a CSR");
    }
    const Sovf = new CSR([1e308, 1e308], [0, 0], [0, 2], 2);
    throwsAny(() => new LinearRegression().fit(Sovf, [1]),
              "linreg refuses an overflow-class CSR reading");
    throwsAny(() => new LogisticRegression().fit(Sovf, [1]),
              "logreg refuses an overflow-class CSR reading");
    Sovf.close();
    const S = CSR.fromDense([[1], [2], [3]]);
    throwsAny(() => new LinearRegression().fit(S, [0, NaN, 1]),
              "linreg refuses NaN in y (CSR path)");
    throwsAny(() => new LogisticRegression().fit(S, [0, NaN, 1]),
              "logreg refuses NaN in y (CSR path)");
    const { X, y } = dataset(41, 60, 5);
    const Sclean = CSR.fromDense(X);
    const a = new LinearRegression().fit(X, y);
    const b = new LinearRegression().fit(Sclean, y);
    close(a.intercept, b.intercept, "sparse fit == dense fit (intercept)", 1e-8);
    assert(a.coef.every((v, j) => Math.abs(v - b.coef[j]) <= 1e-8),
           "sparse fit == dense fit (coef)");
    a.close(); b.close(); Sclean.close();
}

{
    throwsRange(() => new StandardScaler().fitTransform([[1], [NaN]]),
                "StandardScaler.fitTransform refuses NaN");
    throwsRange(() => new MinMaxScaler().fitTransform([[1], [NaN]]),
                "MinMaxScaler.fitTransform refuses NaN");
    const t = new MinMaxScaler().fitTransform([[1, 5], [3, 5], [2, 5]]);
    assert(JSON.stringify(t) === "[[0,0],[1,0],[0.5,0]]",
           "MinMaxScaler fitTransform unchanged on clean data");
}

{
    const r = lcg(202), X = [];
    for (let i = 0; i < 90; i++) {
        const c = i % 3;
        X.push([[0, 6, 3][c] + (r() - 0.5) * 0.4, [0, 0, 5][c] + (r() - 0.5) * 0.4]);
    }
    const g = new GaussianMixture(3, { seed: 42, maxIter: 500 }).fit(X);
    assert(isFinite(g.logLikelihood) && g.logLikelihood > 0,
           "gmm logLikelihood is a finite positive total (blobs data)");
    g.close();
}

{
    const { X, y } = dataset(61, 80, 3);
    const Xr = X.map(row => [row[0], row[1] + row[2]]);
    const r = lcg(62);
    const yr = Xr.map(row => 2 * row[0] - row[1] + (r() - 0.5) * 0.2);
    const Xn = X.map((row, i) => (i % 7 === 0 ? [NaN, row[1], row[2]] : row));
    const ym = yr.map((v, i) => (i % 9 === 0 ? NaN : v));
    const km = new KMeans(3, 5).fit(X);
    const db = new DBScan(0.9, 3).fit(X.map(row => [row[0] * 6, row[1] * 6]));
    const dbX = X.map(row => [row[0] * 6, row[1] * 6]);
    const cases = [
        ["LinearRegression", new LinearRegression().fit(Xr, yr), Xr],
        ["LogisticRegression", new LogisticRegression().fit(X, y), X],
        ["KMeans", km, X],
        ["SVC", new SVC({ kernel: "linear" }).fit(X, y), X],
        ["GaussianMixture", new GaussianMixture(2, { seed: 3 }).fit(X), X],
        ["GaussianNB", new GaussianNB().fit(X, y), X],
        ["DecisionTreeClassifier", new DecisionTreeClassifier({ maxDepth: 3 }).fit(X, y), X],
        ["DecisionTreeRegressor", new DecisionTreeRegressor({ maxDepth: 3 }).fit(Xr, yr), Xr],
        ["RandomForestClassifier", new RandomForestClassifier({ nEstimators: 5, seed: 1 }).fit(X, y), X],
        ["RandomForestRegressor", new RandomForestRegressor({ nEstimators: 5, seed: 1 }).fit(Xr, yr), Xr],
        ["GradientBoostingClassifier", new GradientBoostingClassifier({ nEstimators: 5, seed: 1 }).fit(X, y), X],
        ["GradientBoostingRegressor", new GradientBoostingRegressor({ nEstimators: 5, seed: 1 }).fit(Xr, yr), Xr],
        ["XGBClassifier", new XGBClassifier({ nEstimators: 5, seed: 1 }).fit(Xn, y), Xn],
        ["XGBRegressor", new XGBRegressor({ nEstimators: 5, seed: 1 }).fit(X, yr), X],
        ["KNClassifier", new KNClassifier(3).fit(X, y), X, "predict"],
        ["KNRegressor", new KNRegressor(3).fit(Xr, yr), Xr, "predict"],
        ["PCA", new PCA(2).fit(X), X, "transform"],
        ["StandardScaler", new StandardScaler().fit(X), X, "transform"],
        ["MinMaxScaler", new MinMaxScaler().fit(X), X, "transform"],
    ];
    throwsAny(() => new XGBRegressor().fit(X, ym), "XGB refuses NaN in y");
    throwsAny(() => new RandomForestRegressor().fit(X, ym), "forest refuses NaN in y");
    for (const [name, model, Xin, meth = "predict"] of cases) {
        n++;
        const bytes = model.serialize();
        const loaded = model.constructor.deserialize(bytes);
        const a = JSON.stringify(model[meth](Xin));
        const b = JSON.stringify(loaded[meth](Xin));
        if (a !== b) throw new Error(name + ": round-trip " + meth + " differs\n  " + a + "\n  " + b);
        loaded.close();
        model.close();
    }
    {
        n++;
        const bytes = db.serialize();
        const loaded = DBScan.deserialize(bytes);
        assert(JSON.stringify(db.labels) === JSON.stringify(loaded.labels),
               "DBScan round-trip labels parity");
        loaded.close();
        db.close();
    }
    assert(typeof Pipeline.deserialize === "undefined",
           "Pipeline has no persistence surface");
    const c = new CSR([1], [0], [0, 1], 1);
    assert(typeof c.serialize === "undefined", "CSR has no persistence surface");
    c.close();
}

{
    const m = new LogisticRegression();
    m.fit([[1, 2], [2, 1], [5, 5], [6, 4], [3, 3], [4, 6]], [0, 0, 1, 1, 0, 1]);
    const bytes = m.serialize();
    m.close();
    throwsAny(() => LogisticRegression.deserialize(new Uint8Array(0)),
              "empty record refused");
    throwsAny(() => LogisticRegression.deserialize(bytes.slice(0, 9)),
              "truncated header refused");
    for (let cut = 1; cut < bytes.length; cut += 7) {
        const t = bytes.slice(0, bytes.length - cut);
        let refused = false;
        try { LogisticRegression.deserialize(t); } catch (e) { refused = true; }
        if (!refused) throw new Error("truncated record (" + cut + " bytes cut) accepted");
    }
    n++;
    let loaded = 0;
    for (let i = 0; i < bytes.length; i++) {
        for (const bit of [0x01, 0x80]) {
            const f = bytes.slice();
            f[i] ^= bit;
            try {
                const mm = LogisticRegression.deserialize(f);
                mm.predict([[1, 2]]);
                mm.close();
                loaded++;
            } catch (e) {  }
        }
    }
    assert(loaded <= 2 * bytes.length, "bit-flip sweep terminated");
    const k = new KMeans(2, 1).fit([[1], [2], [3]]);
    const kb = k.serialize();
    k.close();
    throwsAny(() => LogisticRegression.deserialize(kb), "cross-class record refused");
}

{
    const m = new LogisticRegression();
    m.fit([[1, 2], [2, 1], [5, 5], [6, 4]], [0, 0, 1, 1]);
    m.close();
    m.close();
    n++;
    throwsAny(() => m.predict([[1, 2]]), "predict after close throws");
    throwsAny(() => m.coef, "getter after close throws");
    const scaler = new StandardScaler();
    const clf = new DecisionTreeClassifier({ maxDepth: 2 });
    const p = new Pipeline([scaler, clf]);
    const X = [[1, 2], [2, 1], [5, 5], [6, 4]], y = [0, 0, 1, 1];
    p.fit(X, y);
    p.predict(X);
    p.close();
    p.close();
    n++;
    throwsAny(() => p.predict(X), "pipeline predict after close throws");
    const t = scaler.transform(X);
    assert(t.length === 4, "pipeline close leaves stages open (by-reference ownership)");
    scaler.close();
    clf.close();
}

{
    const X = [], y = [];
    const r = lcg(81);
    for (let i = 0; i < 60; i++) {
        X.push([r() * 4 - 2, r() * 4 - 2]);
        y.push(X[i][0] > 0 ? 1 : 0);
    }
    let open = 0;
    const factory = () => {
        const m = new LogisticRegression();
        open++;
        const origClose = m.close.bind(m);
        m.close = () => { if (open > 0) open--; origClose(); };
        return m;
    };
    const scores = crossValScore(factory, X, y, { k: 5 });
    assert(scores.length === 5, "crossValScore returned k scores");
    assert(open === 0, "crossValScore closed every fold estimator");
    const gs = gridSearch(factory, X, y, { maxIter: [50, 100] }, { k: 3 });
    assert(gs.results.length === 2 && gs.best && typeof gs.bestScore === "number",
           "gridSearch structure");
    assert(open === 0, "gridSearch closed every (point, fold) estimator");
}

{
    const { X, y } = dataset(71, 60, 2);
    const Xn = [[NaN, 1]];
    const refusesAtPredict = (m) => {
        try { m.predict(Xn); return false; } catch (e) { return true; }
        finally { try { m.close(); } catch (e) { } }
    };
    assert(refusesAtPredict(new KMeans(2).fit(X)), "KMeans refuses NaN at predict");
    assert(refusesAtPredict(new GaussianNB().fit(X, y)), "GaussianNB refuses NaN at predict");
    assert(refusesAtPredict(new KNClassifier(3).fit(X, y)), "KNClassifier refuses NaN at predict");
    assert(refusesAtPredict(new SVC().fit(X, y)), "SVC refuses NaN at predict");
    assert(refusesAtPredict(new GaussianMixture(2, { seed: 1 }).fit(X)),
           "GaussianMixture refuses NaN at predict");
    {
        const m = new LinearRegression().fit([[1], [2], [3], [4]], [1, 2, 3, 4]);
        let refused = false;
        try { m.predict([[NaN]]); } catch (e) { refused = e instanceof RangeError; }
        assert(refused,
               "LinearRegression refuses NaN at predict (gate, API parity with KMeans/GaussianNB/KN/SVC/GMM)");
        m.close();
    }
    {
        const m = new XGBClassifier({ nEstimators: 5, seed: 1 }).fit(X, y);
        const p = m.predict(Xn);
        assert(Number.isFinite(p[0]),
               "XGB routes NaN to the missing bin (finite prediction)");
        m.close();
    }
}

{
    confusionMatrix([4095], [4095]);
    n++;
    throwsRange(() => confusionMatrix([4096], [0]), "confusionMatrix label cap is 4095");
    close(rocAuc([1, 0, 1, 0], [5, 5, 5, 5]), 0.5, "rocAuc all-tied is exactly 0.5");
    close(rocAuc([1, 1, 0, 0, 0], [3, 5, 1, 3, 4]), 0.75, "rocAuc tie-averaged ranks");
    for (const [N, k] of [[10, 3], [7, 7], [9, 4], [33, 5]]) {
        const folds = kFold(N, { k, shuffle: true, seed: 8 });
        const inTest = new Array(N).fill(0);
        for (const f of folds) {
            const mark = new Array(N).fill(0);
            for (const i of f.test) { inTest[i]++; mark[i]++; }
            for (const i of f.train) mark[i]++;
            if (!mark.every(c => c === 1))
                throw new Error("kFold(" + N + "," + k + "): train is not the complement");
        }
        if (!inTest.every(c => c === 1))
            throw new Error("kFold(" + N + "," + k + "): test folds do not cover exactly once");
        n++;
    }
}

print("test_ml_audit: all " + n + " tests passed");
