// covers: dyna:ml.LogisticRegression
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_logistic_regression_weights");
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

await __kitS.section("all-ones is bit-identical", async () => {
    const X = [[0], [1], [2], [3]], y = [0, 0, 1, 1];
    const p = async (m) => (await m.predictProba([[1.5]]))[0][1];

    const plain = await new LogisticRegression().fit(X, y);
    const ones = await new LogisticRegression().fit(X, y, { sampleWeight: [1, 1, 1, 1] });
    assert((await p(plain)) === (await p(ones)), "all-ones is bit-identical");

    const heavy = await new LogisticRegression().fit(X, y, { sampleWeight: [9, 9, 1, 1] });
    assert((await p(heavy)) < (await p(plain)) - 0.1,
        "weighting the negative class down-weights the positive probability");

    const A = await new LogisticRegression({ maxIter: 4000 })
        .fit([[0], [1], [2], [3], [0], [1]], [0, 0, 1, 1, 0, 0]);
    const B = await new LogisticRegression({ maxIter: 4000 })
        .fit(X, y, { sampleWeight: [2, 2, 1, 1] });
    close(await p(A), await p(B), 1e-6, "weight 2 == the rows appearing twice");

    const scaled = await new LogisticRegression().fit(X, y, { sampleWeight: [3, 3, 3, 3] });
    close(await p(scaled), await p(plain), 1e-12, "a uniform scale factor is a no-op");

    const cw = await new LogisticRegression({ classWeight: "balanced" })
        .fit(X, y, { sampleWeight: [1, 1, 1, 1] });
    assert(Number.isFinite(await p(cw)), "classWeight and sampleWeight compose");
});
__kitS.end();

print("test_ml_weights: all " + n + " assertions passed");
