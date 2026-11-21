// covers: dyna:ml.PCA
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_pca_weights");
import { LinearRegression, LogisticRegression, KMeans, PCA, GaussianNB, DecisionTreeClassifier, DecisionTreeRegressor, RandomForestRegressor, GradientBoostingRegressor, KNClassifier, KNRegressor, DBScan, StandardScaler, MinMaxScaler, SVC, GaussianMixture } from "dyna:ml";

let n = 0;
function assert(c, msg) { n++; if (!c) throw new Error("assertion failed: " + msg); }
function close(a, b, tol, msg) {
    n++;
    if (!(Math.abs(a - b) <= tol))
        throw new Error("assertion failed: " + msg + " (" + a + " vs " + b + ")");
}
async function throwsWith(fn, needle, msg) {
    n++;
    let e = null;
    try { await fn(); } catch (err) { e = err; }
    if (e === null) throw new Error("assertion failed: " + msg + " (expected a throw)");
    if (!String(e.message).includes(needle))
        throw new Error("assertion failed: " + msg + " (message was: " + e.message + ")");
}

await __kitS.section("no weighted fit", async () => {
    const X = [[1, 2], [2, 3], [3, 4], [4, 5]], y = [0, 0, 1, 1];
    const yr = [1, 2, 3, 4];
    const w = { sampleWeight: [1, 1, 1, 1] };

    await throwsWith(async () => await new PCA(1).fit(X, w), "no weighted fit", "PCA refuses");
    await throwsWith(async () => await new PCA(1).fitTransform(X, w), "no weighted fit", "PCA.fitTransform refuses");
    await throwsWith(async () => await new KNClassifier(1).fit(X, y, w), "no weighted fit",
        "KNClassifier refuses");
    await throwsWith(async () => await new KNRegressor(1).fit(X, yr, w), "no weighted fit",
        "KNRegressor refuses");
    await throwsWith(async () => await new DBScan(1, 1).fit(X, w), "no weighted fit",
        "DBScan refuses");
    await throwsWith(async () => await new SVC().fit(X, y, w), "no weighted fit", "SVC refuses");
    await throwsWith(async () => await new GaussianMixture(2).fit(X, w), "no weighted fit",
        "GaussianMixture refuses");
    await throwsWith(async () => await new MinMaxScaler().fit(X, w), "order statistics",
        "MinMaxScaler refuses, and explains why");

    let msg = "";
    try { await new PCA(1).fit(X, w); } catch (e) { msg = e.message; }
    assert(msg.includes("Resample") && msg.includes("LinearRegression"),
        "the error says what to do instead: " + msg);

    assert((await (await new PCA(1).fit(X)).transform(X)).length === 4, "PCA still fits unweighted");
    assert((await (await new SVC().fit(X, y)).predict(X)).length === 4, "SVC still fits unweighted");
    assert((await (await new MinMaxScaler().fit(X)).transform(X)).length === 4,
        "MinMaxScaler still fits unweighted");
});
__kitS.end();

print("test_ml_weights: all " + n + " assertions passed");
