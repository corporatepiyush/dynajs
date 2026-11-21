// covers: dyna:ml.GradientBoostingRegressor
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_gradient_boosting_regressor");
import {
    GradientBoostingClassifier, GradientBoostingRegressor,
    DecisionTreeClassifier, accuracy, logLoss, rocAuc,
} from "dyna:ml";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}
function eq(got, want, msg) {
    n++;
    if (got !== want)
        throw new Error("assertion failed: " + msg + "\n  got:  " + got +
                        "\n  want: " + want);
}
function near(got, want, tol, msg) {
    n++;
    if (!(Math.abs(got - want) <= tol))
        throw new Error("assertion failed: " + msg + "\n  got:  " + got +
                        "\n  want: " + want + " (tol " + tol + ")");
}
async function throws(fn, msg) {
    n++;
    try { await fn(); } catch (e) { return; }
    throw new Error("assertion failed: " + msg + " did not throw");
}

function rng(seed) {
    let s = seed >>> 0;
    return () => { s = (Math.imul(s, 1103515245) + 12345) >>> 0; return s / 4294967296; };
}

function blobs(rows, k, seed, spread) {
    const r = rng(seed), X = [], y = [];
    for (let i = 0; i < rows; i++) {
        const c = i % k;
        X.push([c * 2 + r() * spread, c * 2 + r() * spread, r()]);
        y.push(c);
    }
    return [X, y];
}

function noisy(rows, seed) {
    const r = rng(seed), X = [], y = [];
    for (let i = 0; i < rows; i++) {
        const x = r() * 4 - 2;
        const p = 1 / (1 + Math.exp(-1.5 * x));
        X.push([x, r(), r()]);
        y.push(r() < p ? 1 : 0);
    }
    return [X, y];
}

await __kitS.section("the regressor still fits", async () => {
    const r = rng(77), X = [], y = [];
    for (let i = 0; i < 100; i++) { const a = r() * 3, b = r(); X.push([a, b]); y.push(2 * a + b); }
    const g = new GradientBoostingRegressor({ nEstimators: 40, maxDepth: 3,
                                              learningRate: 0.1 });
    await g.fit(X, y);
    const pred = await g.predict(X);
    let sse = 0;
    for (let i = 0; i < y.length; i++) sse += (pred[i] - y[i]) ** 2;
    assert(sse / y.length < 0.05, "the regressor still fits (mse " + sse / y.length + ")");
    const back = GradientBoostingRegressor.deserialize(g.serialize());
    eq(JSON.stringify(await back.predict(X)), JSON.stringify(pred),
       "the regressor round-trips bit-identically through the new field");
});
__kitS.end();

console.log("test_ml_boosting.js: " + n + " assertions passed");
