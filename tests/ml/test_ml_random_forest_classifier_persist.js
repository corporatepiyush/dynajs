// flags: --std
// covers: dyna:ml.RandomForestClassifier
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_random_forest_classifier_persist");
import * as ml from "dyna:ml";
import { CRC32C } from "dyna:hash";
import { Path } from "dyna:file";
import * as std from "std";
const SCRATCH = `${std.getenv("TMPDIR") || "/tmp"}`;

let n = 0;
function assert(c, m) { n++; if (!c) throw new Error("assertion failed: " + m); }
function throws(fn, kind, m) {
    n++;
    try { fn(); } catch (e) {
        if (kind && !(e instanceof kind)) throw new Error((m || "wrong error") + ": " + e);
        return;
    }
    throw new Error((m || "expected a throw") + " but none happened");
}
async function rejects(fn, kind, m) {
    n++;
    let p;
    try { p = fn(); } catch (e) { throw new Error((m || "rejection") + ": threw synchronously instead of rejecting: " + e); }
    if (!(p instanceof Promise)) throw new Error((m || "rejection") + ": did not return a promise");
    try { await p; } catch (e) {
        if (kind && !(e instanceof kind)) throw new Error((m || "wrong error") + ": " + e);
        return;
    }
    throw new Error((m || "expected a rejection") + " but none happened");
}

function lcg(seed) { let s = seed >>> 0; return () => (s = (s * 1664525 + 1013904223) >>> 0) / 4294967296; }
const rnd = lcg(20260727);
const X = [], y = [], ycls = [];
for (let i = 0; i < 200; i++) {
    const a = rnd() * 4 - 2, b = rnd() * 4 - 2, c = rnd() * 4 - 2;
    X.push([a, b, c]);
    y.push(1.5 * a - 2 * b + 0.5 * c + 0.3);
    ycls.push(a + b > 0 ? 1 : 0);
}
const Xt = X.slice(0, 40);

const MODELS = [
    ["LinearRegression", async () => { const m = new ml.LinearRegression(); await m.fit(X, y); return m; },
     async m => [await m.predict(Xt), m.coef, m.intercept]],
    ["LogisticRegression", async () => { const m = new ml.LogisticRegression(); await m.fit(X, ycls); return m; },
     async m => [await m.predict(Xt), m.coef, m.intercept]],
    ["KMeans", async () => { const m = new ml.KMeans(3, { seed: 7 }); await m.fit(X); return m; },
     async m => [await m.predict(Xt), m.centroids, m.inertia]],
    ["PCA", async () => { const m = new ml.PCA(2); await m.fit(X); return m; },
     async m => [await m.transform(Xt), m.components, m.explainedVarianceRatio]],
    ["GaussianNB", async () => { const m = new ml.GaussianNB(); await m.fit(X, ycls); return m; },
     async m => [await m.predict(Xt), await m.predictProba(Xt)]],
    ["KNClassifier", async () => { const m = new ml.KNClassifier(5); await m.fit(X, ycls); return m; },
     async m => await m.predict(Xt)],
    ["KNRegressor", async () => { const m = new ml.KNRegressor(5); await m.fit(X, y); return m; },
     async m => await m.predict(Xt)],
    ["StandardScaler", async () => { const m = new ml.StandardScaler(); await m.fit(X); return m; },
     async m => [await m.transform(Xt), await m.inverseTransform(await m.transform(Xt))]],
    ["MinMaxScaler", async () => { const m = new ml.MinMaxScaler(); await m.fit(X); return m; },
     async m => [await m.transform(Xt), await m.inverseTransform(await m.transform(Xt))]],
    ["SVC", async () => { const m = new ml.SVC({ kernel: "rbf" }); await m.fit(X, ycls); return m; },
     async m => await m.predict(Xt)],
    ["GaussianMixture", async () => { const m = new ml.GaussianMixture(2, { seed: 3 }); await m.fit(X); return m; },
     async m => [await m.predict(Xt), m.logLikelihood, m.weights, m.means]],
    ["DecisionTreeClassifier", async () => { const m = new ml.DecisionTreeClassifier(); await m.fit(X, ycls); return m; },
     async m => await m.predict(Xt)],
    ["DecisionTreeRegressor", async () => { const m = new ml.DecisionTreeRegressor({ maxDepth: 4 }); await m.fit(X, y); return m; },
     async m => await m.predict(Xt)],
    ["RandomForestClassifier", async () => { const m = new ml.RandomForestClassifier({ nEstimators: 8, seed: 1 }); await m.fit(X, ycls); return m; },
     async m => await m.predict(Xt)],
    ["RandomForestRegressor", async () => { const m = new ml.RandomForestRegressor({ nEstimators: 6, maxDepth: 4, seed: 1 }); await m.fit(X, y); return m; },
     async m => await m.predict(Xt)],
    ["GradientBoostingRegressor", async () => { const m = new ml.GradientBoostingRegressor({ nEstimators: 10, seed: 1 }); await m.fit(X, y); return m; },
     async m => await m.predict(Xt)],
    ["GradientBoostingClassifier", async () => { const m = new ml.GradientBoostingClassifier({ nEstimators: 8, maxDepth: 2, seed: 1 }); await m.fit(X, ycls); return m; },
     async m => [await m.predict(Xt), await m.predictProba(Xt), await m.apply(Xt)]],
    ["XGBRegressor", async () => { const m = new ml.XGBRegressor({ nEstimators: 8, maxDepth: 3, lambda: 2, alpha: 0.5, colsampleByTree: 0.8, seed: 1 }); await m.fit(X, y); return m; },
     async m => [await m.predict(Xt), m.bestRounds]],
    ["XGBClassifier", async () => { const m = new ml.XGBClassifier({ nEstimators: 8, maxDepth: 3, gamma: 0.01, seed: 1 }); await m.fit(X, ycls); return m; },
     async m => [await m.predict(Xt), await m.predictProba(Xt), await m.apply(Xt), m.bestRounds]],
    ["DBScan", async () => { const m = new ml.DBScan(0.7, 4); await m.fit(X); return m; },
     m => [m.labels, m.nClusters]],
];

await __kitS.section("a regressor record loaded as a classifier", async () => {
    const rfr = await MODELS.find(m => m[0] === "RandomForestRegressor")[1]();
    const bytes = rfr.serialize();
    throws(() => ml.RandomForestClassifier.deserialize(bytes), TypeError,
           "a regressor record loaded as a classifier");
    try { ml.RandomForestClassifier.deserialize(bytes); }
    catch (e) {
        assert(/RandomForestRegressor/.test(e.message) &&
               /RandomForestClassifier/.test(e.message),
               "the error names both types, got: " + e.message);
    }
    throws(() => ml.LinearRegression.deserialize(bytes), TypeError,
           "a forest record loaded as a linear model");

    throws(() => ml.KMeans.deserialize(new Uint8Array([68, 89, 78, 83, 1, 0, 1, 0,
                                                       0, 0, 0, 0, 0, 0, 0, 0,
                                                       0, 0, 0, 0, 0, 0, 0, 0])),
           TypeError, "a structures type_id");
});

await __kitS.section("serializing 20 times did not disturb the model", async () => {
    const m = new ml.RandomForestClassifier({ nEstimators: 4, seed: 2 });
    await m.fit(X, ycls);
    const before = JSON.stringify(await m.predict(Xt));
    for (let i = 0; i < 20; i++) m.serialize();
    assert(JSON.stringify(await m.predict(Xt)) === before,
           "serializing 20 times did not disturb the model");

    const bytes = m.serialize();
    m.close();
    throws(() => m.serialize(), Error, "serialize after close");
    const back = ml.RandomForestClassifier.deserialize(bytes);
    assert(JSON.stringify(await back.predict(Xt)) === before,
           "a record outlives the model it came from");
    back.close();
});
__kitS.end();

print("test_ml_persist: all " + n + " assertions passed");
