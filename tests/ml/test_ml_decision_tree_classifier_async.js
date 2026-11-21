// covers: dyna:ml.DecisionTreeClassifier
// dyna:ml threading contract (dynajs.d.ts, module "dyna:ml"): every compute
// call returns a Promise, runs on a thread created for that call and never
// throws synchronously; inputs are snapshotted; one object has one writer.
import { sections as __kitSections } from "../lib/kit.js";
import * as ml from "dyna:ml";
const __kitS = __kitSections("test_ml_decision_tree_classifier_async", { chain: true });
let X, y, yc, make, UNSUPERVISED, classes;

let n = 0, failed = 0;
function ok(cond, what, detail) {
    n++;
    if (!cond) { failed++; console.log("  FAIL  " + what + (detail === undefined ? "" : "  [" + detail + "]")); }
}
async function rejectsWith(p, ctor, what) {
    let e = null;
    try { await p; } catch (x) { e = x; }
    ok(e instanceof ctor, what, e ? e.constructor.name + ": " + e.message : "fulfilled");
    return e;
}
const spawned = () => ml.threadStats().spawned;

const rows = 400, cols = 4;
const Xrows = [];

// 1. Every estimator method returns a Promise, costs exactly one thread, and
//    leaves none alive. The list is enumerated from the module, so a class
//    added later is covered without editing this file.
const ASYNC_METHODS = ["fit", "predict", "predictInto", "predictProba", "transform", "transformInto",
    "inverseTransform", "fitTransform", "decisionFunction", "apply"];
const CLASSIFIER = /Classifier|Logistic|SVC|NB/;
__kitS.section("fixture: build the training matrix and labels", () => {
X = new Float64Array(rows * cols), y = new Float64Array(rows), yc = new Float64Array(rows);
for (let i = 0; i < rows; i++) {
    let s = 0;
    for (let j = 0; j < cols; j++) { const v = Math.sin(i * 0.7 + j * 1.3) + (i % 5) * 0.1; X[i * cols + j] = v; s += v * (j + 1); }
    y[i] = s;
    yc[i] = s > 2.5 ? 1 : 0;
}
for (let i = 0; i < rows; i++) Xrows.push(Array.from(X.subarray(i * cols, (i + 1) * cols)));
make = {
    LinearRegression: () => new ml.LinearRegression(),
    LogisticRegression: () => new ml.LogisticRegression({ maxIter: 30 }),
    KMeans: () => new ml.KMeans(3, 1),
    SVC: () => new ml.SVC({ maxIter: 20 }),
    GaussianMixture: () => new ml.GaussianMixture(2, { seed: 1, maxIter: 10 }),
    GaussianNB: () => new ml.GaussianNB(),
    DecisionTreeClassifier: () => new ml.DecisionTreeClassifier({ maxDepth: 4 }),
    DecisionTreeRegressor: () => new ml.DecisionTreeRegressor({ maxDepth: 4 }),
    RandomForestClassifier: () => new ml.RandomForestClassifier({ nEstimators: 4, maxDepth: 4, seed: 1 }),
    RandomForestRegressor: () => new ml.RandomForestRegressor({ nEstimators: 4, maxDepth: 4, seed: 1 }),
    GradientBoostingRegressor: () => new ml.GradientBoostingRegressor({ nEstimators: 4, maxDepth: 3, seed: 1 }),
    GradientBoostingClassifier: () => new ml.GradientBoostingClassifier({ nEstimators: 4, maxDepth: 3, seed: 1 }),
    XGBRegressor: () => new ml.XGBRegressor({ nEstimators: 4, maxDepth: 3, seed: 1 }),
    XGBClassifier: () => new ml.XGBClassifier({ nEstimators: 4, maxDepth: 3, seed: 1 }),
    PCA: () => new ml.PCA(2),
    KNClassifier: () => new ml.KNClassifier(3),
    KNRegressor: () => new ml.KNRegressor(3),
    DBScan: () => new ml.DBScan(0.5, 3),
    StandardScaler: () => new ml.StandardScaler(),
    MinMaxScaler: () => new ml.MinMaxScaler(),
};
UNSUPERVISED = new Set(["KMeans", "GaussianMixture", "PCA", "DBScan", "StandardScaler", "MinMaxScaler"]);
classes = Object.getOwnPropertyNames(ml).filter((k) => typeof ml[k] === "function" && ml[k].prototype && typeof ml[k].prototype.fit === "function" && k !== "Pipeline");
});

{
let yp, FUNCS, exported, SYNC, SEARCH, cv, gs, cv2;
await __kitS.section("has no call recipe in this test (add one)", async () => {
    yp = Array.from(yc);
    FUNCS = {
        meanSquaredError: [y, y], meanAbsoluteError: [y, y], r2Score: [y, y], logLoss: [yc, yp.map((v) => v ? 0.9 : 0.1)],
        accuracy: [yc, yp], confusionMatrix: [yc, yp], precision: [yc, yp], recall: [yc, yp], f1: [yc, yp],
        specificity: [yc, yp], balancedAccuracy: [yc, yp], matthewsCorrcoef: [yc, yp], cohenKappa: [yc, yp],
        fbeta: [yc, yp, 2], rocAuc: [yc, yp], averagePrecision: [yc, yp],
        trainTestSplit: [rows, { testSize: 0.25, seed: 1 }], kFold: [rows, { k: 4, seed: 1 }],
        stratifiedKFold: [yp, { k: 4, seed: 1 }], imputeMean: [Xrows], dropMissing: [Xrows],
    };
    exported = Object.getOwnPropertyNames(ml).filter((k) => typeof ml[k] === "function" && !(ml[k].prototype && Object.getOwnPropertyNames(ml[k].prototype).length > 1));
    SYNC = new Set(["threadStats"]);
    SEARCH = new Set(["crossValScore", "gridSearch", "randomSearch"]);
    for (const name of exported) {
        if (SYNC.has(name) || SEARCH.has(name)) continue;
        if (!FUNCS[name]) { ok(false, "module function " + name + " has no call recipe in this test (add one)"); continue; }
        const before = spawned();
        const p = ml[name](...FUNCS[name]);
        ok(p instanceof Promise, name + " returns a Promise");
        let err = null;
        try { await p; } catch (e) { err = e; }
        ok(err === null, name + " fulfils on valid input", err && err.message);
        ok(spawned() - before === 1, name + " used exactly one thread", spawned() - before);
        let threw = false, bad;
        try { bad = ml[name](); } catch (e) { threw = true; }
        ok(!threw && bad instanceof Promise, name + "() returns a Promise instead of throwing");
        await rejectsWith(bad, Error, name + "() with no arguments rejects");
    }
});
await __kitS.section("crossValScore returns a Promise", async () => {
    cv = ml.crossValScore(() => new ml.DecisionTreeClassifier({ maxDepth: 3 }), Xrows, yp, { k: 3, seed: 1 });
    ok(cv instanceof Promise, "crossValScore returns a Promise");
    ok((await cv).length === 3, "crossValScore resolves to one score per fold");
});
await __kitS.section("gridSearch resolves to its result table", async () => {
    gs = await ml.gridSearch((p) => new ml.DecisionTreeClassifier(p), Xrows, yp, { maxDepth: [1, 3] }, { k: 3, seed: 1 });
    ok(gs.results.length === 2 && typeof gs.bestScore === "number", "gridSearch resolves to its result table");
});
await __kitS.section("an async scorer and an async factory are awaited", async () => {
    // an async scorer and an async factory are awaited
    cv2 = await ml.crossValScore(async () => new ml.DecisionTreeClassifier({ maxDepth: 3 }), Xrows, yp,
        { k: 3, seed: 1, scoring: async (a, b) => await ml.accuracy(a, b) });
    ok(cv2.length === 3 && cv2.every((s) => s >= 0 && s <= 1), "crossValScore awaits an async factory and an async scorer");
});
}
__kitS.end();
if (!__kitS.listing) {

console.log("test_ml_async: " + (n - failed) + " passed, " + failed + " failed");
if (failed) throw new Error("test_ml_async: " + failed + " failures");
}
