// covers: dyna:ml.threadStats
// dyna:ml threading contract (dynajs.d.ts, module "dyna:ml"): every compute
// call returns a Promise, runs on a thread created for that call and never
// throws synchronously; inputs are snapshotted; one object has one writer.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_thread_stats_async");
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

ok(classes.length >= 20, "the module exposes the estimator classes", classes.length);

let methodsSeen = 0;

for (const name of classes) {
    if (!make[name]) { ok(false, name + " has no constructor recipe in this test (add one)"); continue; }
    const m = make[name]();
    const target = CLASSIFIER.test(name) ? yc : y;
    const fitArgs = UNSUPERVISED.has(name) ? [X, rows, cols] : [X, target, rows, cols];
    let before = spawned();
    const fp = m.fit(...fitArgs);
    ok(fp instanceof Promise, name + ".fit returns a Promise");
    ok((await fp) === m, name + ".fit resolves to the model");
    ok(spawned() - before === 1, name + ".fit used exactly one thread", spawned() - before);
    for (const meth of ASYNC_METHODS) {
        if (meth === "fit" || typeof m[meth] !== "function") continue;
        methodsSeen++;
        if (meth === "predictProba" && /Regressor/.test(name)) {
            await rejectsWith(m.predictProba(X, rows, cols), Error, name + ".predictProba rejects: regressors have no probabilities");
            continue;
        }
        let args;
        if (meth === "predictInto") args = [new Float64Array(rows), X, rows, cols];
        else if (meth === "transformInto") args = [new Float64Array(rows * cols), X, rows, cols];
        else if (meth === "inverseTransform") args = [await m.transform(X, rows, cols), rows, name === "PCA" ? 2 : cols];
        else args = [X, rows, cols];
        before = spawned();
        const p = m[meth](...args);
        ok(p instanceof Promise, name + "." + meth + " returns a Promise");
        let err = null;
        try { await p; } catch (e) { err = e; }
        ok(err === null, name + "." + meth + " fulfils on valid input", err && err.message);
        ok(spawned() - before === 1, name + "." + meth + " used exactly one thread", spawned() - before);
    }
    // Bad arguments reject; they never throw out of the call itself.
    let threw = false, p2;
    try { p2 = m.fit(); } catch (e) { threw = true; }
    ok(!threw && p2 instanceof Promise, name + ".fit() with no arguments returns a Promise instead of throwing");
    await rejectsWith(p2, Error, name + ".fit() with no arguments rejects");
    m.close();
}

ok(methodsSeen >= 40, "the per-method sweep reached the method surface", methodsSeen);

ok(ml.threadStats().live === 0, "no ML thread outlives its call", ml.threadStats().live);

ok(ml.threadStats().inline === 0, "no call fell back to running inline", ml.threadStats().inline);

await __kitS.section("six unreferenced models fitted concurrently give identical predictions", async () => {
    const pending = [];
    for (let k = 0; k < 6; k++)
        pending.push(new ml.DecisionTreeRegressor({ maxDepth: 6 }).fit(X, y, rows, cols).then((m) => m.predict(X, rows, cols)));
    let junk = [];
    for (let i = 0; i < 20000; i++) junk.push({ i, s: "x" + i });
    junk = null;
    const outs = await Promise.all(pending);
    let all = outs.every((o) => o.length === rows);
    for (let k = 1; k < outs.length && all; k++)
        for (let i = 0; i < rows && all; i++) all = outs[k][i] === outs[0][i];
    ok(all, "six unreferenced models fitted concurrently give identical predictions");
    ok(ml.threadStats().live === 0, "every thread has ended", ml.threadStats().live);
});

await __kitS.section("a concurrent metric returned a wrong value", async () => {
    const a = [1, 0, 1, 1, 0, 1, 0, 0], b = [1, 0, 0, 1, 0, 1, 1, 0];
    let settled = 0;
    const before = spawned();
    for (let round = 0; round < 400; round++) {
        const batch = [];
        for (let k = 0; k < 12; k++) batch.push(ml.accuracy(a, b).then((v) => { settled++; return v; }));
        const vals = await Promise.all(batch);
        if (!vals.every((v) => v === 0.75)) { ok(false, "a concurrent metric returned a wrong value", vals.join(",")); break; }
    }
    ok(settled === 4800, "4800 short concurrent calls all settled", settled);
    ok(spawned() - before === 4800, "each of them had its own thread", spawned() - before);
    ok(ml.threadStats().live === 0, "and none is left", ml.threadStats().live);
});

await __kitS.section("concurrent fits all settled with a finite inertia", async () => {
    const R = 20000, C = 6, K = 300;
    const BX = new Float64Array(R * C);
    for (let i = 0; i < R * C; i++) BX[i] = Math.sin(i * 0.61) + (i % 7);
    const before = ml.threadStats();
    let peak = 0;
    const watch = setInterval(() => { peak = Math.max(peak, ml.threadStats().live); }, 1);
    const all = [];
    for (let k = 0; k < K; k++) all.push(new ml.KMeans(4, k).fit(BX, R, C).then((m) => { const v = m.inertia; m.close(); return v; }));
    peak = Math.max(peak, ml.threadStats().live);
    const inertias = await Promise.all(all);
    clearInterval(watch);
    const after = ml.threadStats();
    ok(inertias.length === K && inertias.every((v) => Number.isFinite(v) && v > 0), K + " concurrent fits all settled with a finite inertia");
    ok(after.spawned - before.spawned === K, "each of the " + K + " fits had a thread of its own", after.spawned - before.spawned);
    ok(after.inline === before.inline, "none of them ran on the calling thread", after.inline - before.inline);
    ok(peak <= 128, "no more than 128 compute threads were alive at once", peak);
    // How far the fits overlap depends on how busy the machine is, so reaching
    // the ceiling is reported, not required: on a loaded host the bound above
    // holds without ever being pressed.
    if (peak < 64) console.log("  NOTE  the thread ceiling was not pressed on this run (peak " + peak + " alive at once)");
    ok(after.live === 0, "no thread is left", after.live);
});
__kitS.end();

console.log("test_ml_async: " + (n - failed) + " passed, " + failed + " failed");
if (failed) throw new Error("test_ml_async: " + failed + " failures");
