// covers: dyna:ml.LinearRegression
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_linear_regression_production");
import * as ml from "dyna:ml";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}
function eq(got, want, msg) {
    n++;
    if (got !== want)
        throw new Error("assertion failed: " + msg + "\n  got:  " + got +
                        "\n  want: " + want);
}
function near(got, want, tol, msg) {
    n++;
    if (!(Math.abs(got - want) <= tol))
        throw new Error("assertion failed: " + msg + "\n  got:  " + got +
                        "\n  want: " + want);
}
async function throws(fn, msg) {
    n++;
    try { await fn(); } catch (e) { return e; }
    throw new Error("assertion failed: " + msg + " did not throw");
}

function rng(seed) {
    let s = seed >>> 0;
    return () => { s = (Math.imul(s, 1103515245) + 12345) >>> 0; return s / 4294967296; };
}
function blobs(rows, k, seed, spread) {
    const r = rng(seed), X = [], y = [];
    for (let i = 0; i < rows; i++) {
        const c = i % k;
        X.push([c * spread + r() * 2, c * spread + r() * 2, r()]);
        y.push(c);
    }
    return [X, y];
}

await __kitS.section("rejects NaN in X", async () => {
    const [X, y] = blobs(40, 2, 3, 3);
    const bad = X.map((r) => r.slice());
    bad[7][1] = NaN;
    const supervised = [
        ["LinearRegression", () => new ml.LinearRegression()],
        ["LogisticRegression", () => new ml.LogisticRegression()],
        ["DecisionTreeClassifier", () => new ml.DecisionTreeClassifier()],
        ["DecisionTreeRegressor", () => new ml.DecisionTreeRegressor()],
        ["RandomForestClassifier", () => new ml.RandomForestClassifier({ nEstimators: 2 })],
        ["RandomForestRegressor", () => new ml.RandomForestRegressor({ nEstimators: 2 })],
        ["GradientBoostingRegressor", () => new ml.GradientBoostingRegressor({ nEstimators: 2 })],
        ["GradientBoostingClassifier", () => new ml.GradientBoostingClassifier({ nEstimators: 2 })],
        ["GaussianNB", () => new ml.GaussianNB()],
        ["KNClassifier", () => new ml.KNClassifier(3)],
        ["KNRegressor", () => new ml.KNRegressor(3)],
        ["SVC", () => new ml.SVC({ maxIter: 20 })],
    ];
    for (const [name, make] of supervised) {
        const e = await throws(async () => await make().fit(bad, y), name + " rejects NaN in X");
        assert(/X\[7\]\[1\]/.test(e.message),
               name + " names the exact cell, not just 'NaN in input': " + e.message);
    }
    const unsupervised = [
        ["KMeans", () => new ml.KMeans(2)],
        ["PCA", () => new ml.PCA(1)],
        ["DBScan", () => new ml.DBScan(1, 2)],
        ["StandardScaler", () => new ml.StandardScaler()],
        ["MinMaxScaler", () => new ml.MinMaxScaler()],
        ["GaussianMixture", () => new ml.GaussianMixture(2, { maxIter: 5 })],
    ];
    for (const [name, make] of unsupervised)
        await throws(async () => await make().fit(bad), name + " rejects NaN in X");

    const inf = X.map((r) => r.slice());
    inf[3][0] = Infinity;
    const e = await throws(async () => await new ml.LinearRegression().fit(inf, y), "rejects Infinity");
    assert(/infinite/.test(e.message), "and says 'infinite', not 'NaN': " + e.message);

    const ybad = y.slice();
    ybad[5] = NaN;
    const ey = await throws(async () => await new ml.LinearRegression().fit(X, ybad), "rejects NaN in y");
    assert(/y\[5\]/.test(ey.message) && /cannot be imputed/.test(ey.message),
           "the y message points at dropping the row: " + ey.message);

    const m = new ml.LinearRegression();
    await m.fit(X, y);
    assert((await m.predict(X)).length === X.length, "clean data still fits");
});
__kitS.end();
console.log("test_ml_production.js: " + n + " assertions passed");
