// flags: --std
// timeout: 300
// Split from tests/test_audit_w2_native_1.js: the ml sections.
import * as ml from "dyna:ml";
import { args } from "dyna:sys";

const BIN = args()[0];
let failures = 0, checks = 0;
function ok(cond, msg) {
    checks++;
    if (!cond) {
        failures++;
        print("  FAIL:", msg);
    }
}
function eq(got, want, msg) {
    ok(Object.is(got, want), msg + " (got " + String(got) + ", want " + String(want) + ")");
}
function errName(fn) {
    try { fn(); return "none"; } catch (e) { return e.name; }
}
async function errNameAsync(fn) {
    try { await fn(); return "none"; } catch (e) { return e.name; }
}
// ---- resource pin (M1b-04) ---------------------------------------------------
{
    let pipe, got = "?";
    const evil = { fit() { got = errName(() => pipe.close()); return this; }, transform(X) { return X; } };
    const last = { fit() { return this; }, predict() { return [0]; } };
    pipe = new ml.Pipeline([evil, evil, last]);
    const outer = await errNameAsync(() => pipe.fit([[1, 2], [3, 4]], [0, 1]));
    eq(got, "TypeError", "a stage cannot close the pipeline that is running it");
    eq(outer, "none", "the pipeline finished its fit");
    eq(pipe.closed, false, "the pipeline is still open");
    pipe.close();
    eq(pipe.closed, true, "close works once no method is running");
}
// ---- ml --------------------------------------------------------------------------
{
    const X = [], y = [];
    for (let i = 0; i < 40; i++) { X.push([i, i * 2 + 1, (i * 7) % 5]); y.push(3 * i + 1); }
    const m = await new ml.LinearRegression().fit(X, y);
    let fired = 0;
    Object.defineProperty(Array.prototype, "1", { configurable: true, set(v) { fired++; m.close(); } });
    let coef;
    try { coef = m.coef; } finally { delete Array.prototype[1]; }
    eq(fired, 0, "fitted-attribute getters never reach an Array.prototype setter (M1b-03)");
    eq(coef.length, 3, "coef is complete");
    m.close();

    let polluted = 0;
    Object.defineProperty(Object.prototype, "train", { configurable: true, get() { polluted++; return [100000, 100001, 100002]; }, set(v) { } });
    let res = "?";
    try {
        const XX = [], yy = [];
        for (let i = 0; i < 10; i++) { XX.push([i, i + 1]); yy.push(i % 2); }
        try { await ml.crossValScore(() => new ml.GaussianNB(), XX, yy, { k: 2 }); res = "ok"; } catch (e) { res = e.name; }
    } finally {
        delete Object.prototype.train;
    }
    eq(polluted, 0, "fold tables are own properties; an Object.prototype accessor is never consulted (M1b-05)");
    ok(res === "ok" || res === "TypeError" || res === "RangeError", "crossValScore completed or refused cleanly (" + res + ")");

    const shared = new Float64Array(new SharedArrayBuffer(8 * 6));
    shared.set([1, 2, 3, 4, 5, 6]);
    eq(await errNameAsync(() => new ml.StandardScaler().fit(shared, 3, 2)), "TypeError", "fit refuses a Float64Array over a SharedArrayBuffer (M1b-06)");
    eq(await errNameAsync(() => ml.CSR.fromDense(shared, 3, 2)), "TypeError", "CSR.fromDense refuses a shared Float64Array");
    eq(await errNameAsync(() => new ml.StandardScaler().fit(new Float64Array(shared), 3, 2)), "none", "a private copy is accepted (control)");

    const pts = [];
    for (let i = 0; i < 60; i++) pts.push([Math.sin(i) * 3 + (i % 3) * 10, Math.cos(i * 1.7) * 3 + (i % 3) * 7]);
    const a7 = (await new ml.KMeans(3, 7).fit(pts)).inertia;
    const o7 = (await new ml.KMeans(3, { seed: 7 }).fit(pts)).inertia;
    eq(o7, a7, "KMeans(k, { seed }) honours the seed it is given (M1b-08)");
    eq(errName(() => new ml.KMeans(3, "7")), "TypeError", "KMeans refuses a non-number seed");
    eq(errName(() => new ml.KMeans(3, { seed: "x" })), "TypeError", "KMeans refuses a non-number { seed }");

    const Xs = [], ys = [];
    for (let i = 0; i < 20; i++) { Xs.push([i, (i * 3) % 7]); ys.push(i < 10 ? 0 : 1); }
    const svc = await new ml.SVC().fit(Xs, ys);
    const flat = new Float64Array(40);
    Xs.forEach((r, i) => { flat[2 * i] = r[0]; flat[2 * i + 1] = r[1]; });
    ok((await svc.decisionFunction(flat, 20, 2)) instanceof Float64Array, "binary SVC.decisionFunction(flat X) is a Float64Array (M1b-09)");
    ok(Array.isArray(await svc.decisionFunction(Xs)), "binary SVC.decisionFunction(rows) stays an array (control)");
}
if (failures) {
    print("test_ml_audit_w2_native_1: " + failures + " of " + checks + " FAILED");
    throw new Error("test_ml_audit_w2_native_1 failed");
}
print("test_ml_audit_w2_native_1: " + checks + " checks passed");
