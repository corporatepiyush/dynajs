// covers: dyna:ml.RandomForestClassifier
// dyna:ml threading contract (dynajs.d.ts, module "dyna:ml"): every compute
// call returns a Promise, runs on a thread created for that call and never
// throws synchronously; inputs are snapshotted; one object has one writer.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_random_forest_classifier_async");
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

await __kitS.section("a timer armed before a long fit fires before it settles", async () => {
    const R = 60000, C = 12;
    const BX = new Float64Array(R * C), By = new Float64Array(R);
    for (let i = 0; i < R * C; i++) BX[i] = Math.sin(i * 0.37);
    for (let i = 0; i < R; i++) By[i] = BX[i * C] > 0 ? 1 : 0;
    const order = [];
    const f = new ml.RandomForestClassifier({ nEstimators: 20, maxDepth: 10, seed: 1 });
    const fit = f.fit(BX, By, R, C).then(() => order.push("fit"));
    let ticks = 0;
    const iv = setInterval(() => { ticks++; }, 1);
    setTimeout(() => order.push("timer"), 0);
    await fit;
    clearInterval(iv);
    ok(order[0] === "timer" && order[1] === "fit", "a timer armed before a long fit fires before it settles", order.join(","));
    ok(ticks >= 2, "an interval keeps firing while the fit is in flight", ticks);

    // 4. One writer: a second fit, a getter and close() are refused mid-fit;
    //    reads overlap freely once fitted.
    const g = new ml.RandomForestClassifier({ nEstimators: 20, maxDepth: 10, seed: 1 });
    const first = g.fit(BX, By, R, C);
    await rejectsWith(g.fit(BX, By, R, C), TypeError, "a second fit on a fitting model rejects");
    await rejectsWith(g.predict(BX, R, C), TypeError, "predict on a fitting model rejects");
    let gerr = null;
    try { g.depth; } catch (e) { gerr = e; }
    ok(gerr instanceof TypeError, "a getter on a fitting model throws", gerr && gerr.message);
    let cerr = null;
    try { g.close(); } catch (e) { cerr = e; }
    ok(cerr instanceof TypeError && /in flight/.test(cerr.message), "close() on a fitting model throws", cerr && cerr.message);
    ok(g.closed === false, "the refused close left the model open");
    await first;
    const before = spawned();
    const [a, b, c] = await Promise.all([g.predict(BX, R, C), g.predict(BX, R, C), g.predict(BX, R, C)]);
    ok(spawned() - before === 3, "three overlapping predicts ran on three threads", spawned() - before);
    let same = a.length === R;
    for (let i = 0; i < R && same; i++) same = a[i] === b[i] && b[i] === c[i];
    ok(same, "overlapping predicts on one model agree element for element");
    const pend = g.predict(BX, R, C);
    await rejectsWith(g.fit(BX, By, R, C), TypeError, "fit while a predict is in flight rejects");
    let c2 = null;
    try { g.close(); } catch (e) { c2 = e; }
    ok(c2 instanceof TypeError, "close() while a predict is in flight throws");
    await pend;
    g.close();
    ok(g.closed === true, "close works once every call has settled");
    f.close();

    // 5. Snapshot: the result does not depend on what happens to the inputs
    //    after the call starts -- overwrite with NaN, then detach.
    const ref = new ml.LinearRegression();
    await ref.fit(BX, By, R, C);
    const want = JSON.stringify([ref.coef, ref.intercept]);
    const X2 = new Float64Array(BX), y2 = new Float64Array(By);
    const m2 = new ml.LinearRegression();
    const p = m2.fit(X2, y2, R, C);
    X2.fill(NaN);
    y2.fill(NaN);
    X2.buffer.transfer();
    await p;
    ok(JSON.stringify([m2.coef, m2.intercept]) === want, "overwriting and detaching X and y after the call started changes nothing");
    // 6. predictInto writes `out` only at fulfilment, and never into a buffer
    //    that went away meanwhile.
    const out = new Float64Array(R).fill(-7);
    const pi = ref.predictInto(out, BX, R, C);
    ok(out[0] === -7 && out[R - 1] === -7, "out is untouched while predictInto is in flight");
    ok((await pi) === R, "predictInto resolves to the row count");
    const direct = await ref.predict(BX, R, C);
    let eq = true;
    for (let i = 0; i < R && eq; i++) eq = Object.is(out[i], direct[i]);
    ok(eq, "predictInto filled out with the same values predict returns");
    const gone = new Float64Array(R);
    const pg = ref.predictInto(gone, BX, R, C);
    gone.buffer.transfer();
    const ge = await rejectsWith(pg, Error, "predictInto into a buffer detached mid-flight rejects");
    ok(ge && !/undefined/.test(String(ge.message)), "the detached-out rejection carries a message", ge && ge.message);
    const small = new Float64Array(R - 1);
    const re = await rejectsWith(ref.predictInto(small, BX, R, C), RangeError, "a short out rejects with RangeError");
    ok(re && /nothing was written/.test(re.message), "the short-out rejection says nothing was written", re && re.message);
    ref.close();
    m2.close();
});
__kitS.end();

console.log("test_ml_async: " + (n - failed) + " passed, " + failed + " failed");
if (failed) throw new Error("test_ml_async: " + failed + " failures");
