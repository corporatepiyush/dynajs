// covers: dyna:ml.DBScan
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_db_scan_audit");
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
async function throwsWhich(fn, ctor, m) {
    n++;
    try { await fn(); } catch (e) {
        if (e instanceof ctor) return;
        throw new Error("assertion failed (wrong error): " + m + " -> " + e);
    }
    throw new Error("assertion failed (expected " + ctor.name + "): " + m);
}
const throwsRange = async (fn, m) => await throwsWhich(fn, RangeError, m);
const throwsAny = async (fn, m) => await throwsWhich(fn, Error, m);
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

await __kitS.section("XGB refuses NaN in y", async () => {
    const { X, y } = dataset(61, 80, 3);
    const Xr = X.map(row => [row[0], row[1] + row[2]]);
    const r = lcg(62);
    const yr = Xr.map(row => 2 * row[0] - row[1] + (r() - 0.5) * 0.2);
    const Xn = X.map((row, i) => (i % 7 === 0 ? [NaN, row[1], row[2]] : row));
    const ym = yr.map((v, i) => (i % 9 === 0 ? NaN : v));
    const km = await new KMeans(3, 5).fit(X);
    const db = await new DBScan(0.9, 3).fit(X.map(row => [row[0] * 6, row[1] * 6]));
    const dbX = X.map(row => [row[0] * 6, row[1] * 6]);
    const cases = [
        ["LinearRegression", await new LinearRegression().fit(Xr, yr), Xr],
        ["LogisticRegression", await new LogisticRegression().fit(X, y), X],
        ["KMeans", km, X],
        ["SVC", await new SVC({ kernel: "linear" }).fit(X, y), X],
        ["GaussianMixture", await new GaussianMixture(2, { seed: 3 }).fit(X), X],
        ["GaussianNB", await new GaussianNB().fit(X, y), X],
        ["DecisionTreeClassifier", await new DecisionTreeClassifier({ maxDepth: 3 }).fit(X, y), X],
        ["DecisionTreeRegressor", await new DecisionTreeRegressor({ maxDepth: 3 }).fit(Xr, yr), Xr],
        ["RandomForestClassifier", await new RandomForestClassifier({ nEstimators: 5, seed: 1 }).fit(X, y), X],
        ["RandomForestRegressor", await new RandomForestRegressor({ nEstimators: 5, seed: 1 }).fit(Xr, yr), Xr],
        ["GradientBoostingClassifier", await new GradientBoostingClassifier({ nEstimators: 5, seed: 1 }).fit(X, y), X],
        ["GradientBoostingRegressor", await new GradientBoostingRegressor({ nEstimators: 5, seed: 1 }).fit(Xr, yr), Xr],
        ["XGBClassifier", await new XGBClassifier({ nEstimators: 5, seed: 1 }).fit(Xn, y), Xn],
        ["XGBRegressor", await new XGBRegressor({ nEstimators: 5, seed: 1 }).fit(X, yr), X],
        ["KNClassifier", await new KNClassifier(3).fit(X, y), X, "predict"],
        ["KNRegressor", await new KNRegressor(3).fit(Xr, yr), Xr, "predict"],
        ["PCA", await new PCA(2).fit(X), X, "transform"],
        ["StandardScaler", await new StandardScaler().fit(X), X, "transform"],
        ["MinMaxScaler", await new MinMaxScaler().fit(X), X, "transform"],
    ];
    await throwsAny(async () => await new XGBRegressor().fit(X, ym), "XGB refuses NaN in y");
    await throwsAny(async () => await new RandomForestRegressor().fit(X, ym), "forest refuses NaN in y");
    for (const [name, model, Xin, meth = "predict"] of cases) {
        n++;
        const bytes = model.serialize();
        const loaded = model.constructor.deserialize(bytes);
        const a = JSON.stringify(await model[meth](Xin));
        const b = JSON.stringify(await loaded[meth](Xin));
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
});
__kitS.end();

print("test_ml_audit: all " + n + " tests passed");
