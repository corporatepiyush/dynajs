// covers: dyna:ml.Pipeline
// dyna:ml threading contract (dynajs.d.ts, module "dyna:ml"): every compute
// call returns a Promise, runs on a thread created for that call and never
// throws synchronously; inputs are snapshotted; one object has one writer.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_pipeline_async");
import * as ml from "dyna:ml";

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
const X = new Float64Array(rows * cols), y = new Float64Array(rows), yc = new Float64Array(rows);
for (let i = 0; i < rows; i++) {
    let s = 0;
    for (let j = 0; j < cols; j++) { const v = Math.sin(i * 0.7 + j * 1.3) + (i % 5) * 0.1; X[i * cols + j] = v; s += v * (j + 1); }
    y[i] = s;
    yc[i] = s > 2.5 ? 1 : 0;
}
const Xrows = [];
for (let i = 0; i < rows; i++) Xrows.push(Array.from(X.subarray(i * cols, (i + 1) * cols)));

// 1. Every estimator method returns a Promise, costs exactly one thread, and
//    leaves none alive. The list is enumerated from the module, so a class
//    added later is covered without editing this file.
const ASYNC_METHODS = ["fit", "predict", "predictInto", "predictProba", "transform", "transformInto",
    "inverseTransform", "fitTransform", "decisionFunction", "apply"];
const make = {
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
const UNSUPERVISED = new Set(["KMeans", "GaussianMixture", "PCA", "DBScan", "StandardScaler", "MinMaxScaler"]);
const CLASSIFIER = /Classifier|Logistic|SVC|NB/;
const classes = Object.getOwnPropertyNames(ml).filter((k) => typeof ml[k] === "function" && ml[k].prototype && typeof ml[k].prototype.fit === "function" && k !== "Pipeline");

await __kitS.section("Pipeline.fit returns a Promise", async () => {
    const pipe = new ml.Pipeline([new ml.StandardScaler(), new ml.LogisticRegression({ maxIter: 50 })]);
    const p = pipe.fit(Xrows, Array.from(yc));
    ok(p instanceof Promise, "Pipeline.fit returns a Promise");
    ok((await p) === pipe, "Pipeline.fit resolves to the pipeline");
    const pred = await pipe.predict(Xrows);
    ok(pred.length === rows, "Pipeline.predict resolves to one label per row");
    const acc = await ml.accuracy(Array.from(yc), pred);
    ok(acc > 0.8, "the pipeline learned the labels", acc);
    pipe.close();

    let seen = 0;
    const slow = {
        async fit(Xin) { await new Promise((r) => setTimeout(r, 5)); seen++; return this; },
        async transform(Xin) { await new Promise((r) => setTimeout(r, 5)); return Xin; },
    };
    const pipe2 = new ml.Pipeline([slow, new ml.GaussianNB()]);
    await pipe2.fit(Xrows, Array.from(yc));
    ok(seen === 1, "an async user stage is awaited before the next stage runs");
    ok((await pipe2.predict(Xrows)).length === rows, "predict runs through the async stage");
    pipe2.close();

    const boom = { fit() { return Promise.reject(new RangeError("stage says no")); }, transform(Xin) { return Xin; } };
    const pipe3 = new ml.Pipeline([boom, new ml.GaussianNB()]);
    const e = await rejectsWith(pipe3.fit(Xrows, Array.from(yc)), RangeError, "a rejecting stage rejects the pipeline with its own error");
    ok(e && e.message === "stage says no", "the stage's rejection reason is passed through unchanged", e && e.message);
    pipe3.close();
});
__kitS.end();

console.log("test_ml_async: " + (n - failed) + " passed, " + failed + " failed");
if (failed) throw new Error("test_ml_async: " + failed + " failures");
