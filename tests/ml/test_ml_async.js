// dyna:ml threading contract (dynajs.d.ts, module "dyna:ml"): every compute
// call returns a Promise, runs on a thread created for that call and never
// throws synchronously; inputs are snapshotted; one object has one writer.
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

// 2. Module functions: Promise, one thread, reject on bad input.
{
    const yp = Array.from(yc);
    const FUNCS = {
        meanSquaredError: [y, y], meanAbsoluteError: [y, y], r2Score: [y, y], logLoss: [yc, yp.map((v) => v ? 0.9 : 0.1)],
        accuracy: [yc, yp], confusionMatrix: [yc, yp], precision: [yc, yp], recall: [yc, yp], f1: [yc, yp],
        specificity: [yc, yp], balancedAccuracy: [yc, yp], matthewsCorrcoef: [yc, yp], cohenKappa: [yc, yp],
        fbeta: [yc, yp, 2], rocAuc: [yc, yp], averagePrecision: [yc, yp],
        trainTestSplit: [rows, { testSize: 0.25, seed: 1 }], kFold: [rows, { k: 4, seed: 1 }],
        stratifiedKFold: [yp, { k: 4, seed: 1 }], imputeMean: [Xrows], dropMissing: [Xrows],
    };
    const exported = Object.getOwnPropertyNames(ml).filter((k) => typeof ml[k] === "function" && !(ml[k].prototype && Object.getOwnPropertyNames(ml[k].prototype).length > 1));
    const SYNC = new Set(["threadStats"]);
    const SEARCH = new Set(["crossValScore", "gridSearch", "randomSearch"]);
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
    const cv = ml.crossValScore(() => new ml.DecisionTreeClassifier({ maxDepth: 3 }), Xrows, yp, { k: 3, seed: 1 });
    ok(cv instanceof Promise, "crossValScore returns a Promise");
    ok((await cv).length === 3, "crossValScore resolves to one score per fold");
    const gs = await ml.gridSearch((p) => new ml.DecisionTreeClassifier(p), Xrows, yp, { maxDepth: [1, 3] }, { k: 3, seed: 1 });
    ok(gs.results.length === 2 && typeof gs.bestScore === "number", "gridSearch resolves to its result table");
    // an async scorer and an async factory are awaited
    const cv2 = await ml.crossValScore(async () => new ml.DecisionTreeClassifier({ maxDepth: 3 }), Xrows, yp,
        { k: 3, seed: 1, scoring: async (a, b) => await ml.accuracy(a, b) });
    ok(cv2.length === 3 && cv2.every((s) => s >= 0 && s <= 1), "crossValScore awaits an async factory and an async scorer");
}

// 3. The event loop is served while a fit runs: a zero-delay timer armed
//    before the fit fires before the fit settles. Asserts ORDER, not duration.
{
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
}

// 7. Pipeline: stages run through their own promises; a user stage may be
//    async; a stage that rejects rejects the pipeline.
{
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
}

// 8. Garbage collection while calls are in flight: a model with no other
//    reference still completes, and churn on the calling thread does not
//    disturb the computation.
{
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
}

// 9. Completions that land together: many short calls finishing at once must
//    each be delivered. A lost wake-up is a hang, which the suite timeout
//    reports; the counts prove every call settled.
{
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
}

// 10. Far more calls than the thread ceiling, all at once: at most 128 compute
//     threads are alive together, the rest wait their turn (each still gets a
//     thread of its own), nothing runs on the calling thread, all settle.
{
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
}

console.log("test_ml_async: " + (n - failed) + " passed, " + failed + " failed");
if (failed) throw new Error("test_ml_async: " + failed + " failures");
