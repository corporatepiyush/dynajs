// flags: --std
// covers: dyna:ml.DecisionTreeRegressor
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_decision_tree_regressor_persist");
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

await __kitS.section("non-Error from deserialize", async () => {
    const model = new ml.DecisionTreeRegressor({ maxDepth: 3 });
    await model.fit(X, y);
    const good = model.serialize();
    function repair(b) {
        const crc = CRC32C(b.subarray(0, b.length - 4)) >>> 0;
        for (let i = 0; i < 4; i++) b[b.length - 4 + i] = (crc >>> (i * 8)) & 0xff;
        return b;
    }
    let attempts = 0, decoded = 0, threw = 0;
    const step = Math.max(1, Math.ceil((good.length - 24) / 250));
    for (let i = 20; i < good.length - 4; i += step) {
        for (const mask of [0xff, 0x40]) {
            const b = good.slice();
            b[i] ^= mask;
            repair(b);
            attempts++;
            try {
                const v = ml.DecisionTreeRegressor.deserialize(b);
                decoded++;
                await v.predict(Xt);
            } catch (e) {
                threw++;
                if (!(e instanceof Error)) throw new Error("non-Error from deserialize");
            }
        }
    }
    assert(attempts > 100, "forged-tree sweep ran (" + attempts + " cases)");
    assert(decoded > 0, "the sweep must actually reach the reader");
    print("  forged-tree sweep: " + attempts + " records, " + threw +
          " rejected, " + decoded + " decoded and predicted with");
});
__kitS.end();

print("test_ml_persist: all " + n + " assertions passed");
