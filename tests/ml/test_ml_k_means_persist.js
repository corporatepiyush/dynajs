// flags: --std
// covers: dyna:ml.KMeans
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_k_means_persist");
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

await __kitS.section("unfitted serialize", async () => {
    for (const C of [ml.LinearRegression, ml.KMeans, ml.PCA, ml.GaussianNB,
                     ml.SVC, ml.DecisionTreeClassifier]) {
        const m = C === ml.KMeans ? new C(2) : C === ml.PCA ? new C(1) : new C();
        throws(() => m.serialize(), TypeError, C.name + " unfitted serialize");
        await rejects(() => m.save(new Path("/tmp/should_not_exist.dyns")), TypeError,
               C.name + " unfitted save");
    }
});

await __kitS.section("kmeans record with k=0 refused", async () => {
    function repair(b) {
        const crc = CRC32C(b.subarray(0, b.length - 4)) >>> 0;
        for (let i = 0; i < 4; i++) b[b.length - 4 + i] = (crc >>> (i * 8)) & 0xff;
        return b;
    }
    const le64 = (b, off, v) => { for (let i = 0; i < 8; i++) b[off + i] = (v >>> (i * 8)) & 0xff; };
    const le32 = (b, off, v) => { for (let i = 0; i < 4; i++) b[off + i] = (v >>> (i * 8)) & 0xff; };
    const forged = (model, mutate, why) => {
        const b = model.serialize();
        mutate(b);
        repair(b);
        throws(() => model.constructor.deserialize(b), Error, why);
    };
    const km = new ml.KMeans(3, { seed: 7 }); await km.fit(X);
    forged(km, b => le64(b, 20 + 0, 0), "kmeans record with k=0 refused");
    const svc = new ml.SVC({ kernel: "rbf" }); await svc.fit(X, ycls);
    forged(svc, b => le64(b, 20 + 56, 3), "svm record with ncl/nbin mismatch refused");
    forged(svc, b => le32(b, 20 + 20, 0x7fffffff), "svm record with degree 2^31 refused");
    const dtc = new ml.DecisionTreeClassifier(); await dtc.fit(X, ycls);
    forged(dtc, b => le64(b, 20 + 32, 1), "classifier record with n_classes=1 refused");
    forged(dtc, b => le64(b, 20 + 208, 0), "record with a 0-node tree refused");
    const gmm = new ml.GaussianMixture(2, { seed: 3 }); await gmm.fit(X);
    forged(gmm, b => le64(b, 20 + 0, 0), "gmm record with k=0 refused");
    const nb = new ml.GaussianNB(); await nb.fit(X, ycls);
    forged(nb, b => le64(b, 20 + 8, 0), "nb record with ncl=0 refused");
});
__kitS.end();

print("test_ml_persist: all " + n + " assertions passed");
