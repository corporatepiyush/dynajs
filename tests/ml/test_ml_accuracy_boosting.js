// covers: dyna:ml.accuracy
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_accuracy_boosting");
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

await __kitS.section("-class proba row count", async () => {
for (const K of [2, 3, 4]) {
    const [X, y] = blobs(160, K, 17 + K, 1.4);
    const m = new GradientBoostingClassifier({ nEstimators: 20, maxDepth: 2,
                                               learningRate: 0.3 });
    await m.fit(X, y);
    const pred = await m.predict(X), proba = await m.predictProba(X);
    eq(proba.length, X.length, K + "-class proba row count");
    eq(proba[0].length, K, K + "-class proba column count");
    let disagree = 0;
    for (let i = 0; i < X.length; i++) {
        let s = 0, best = 0;
        for (let c = 0; c < K; c++) {
            s += proba[i][c];
            assert(proba[i][c] >= 0 && proba[i][c] <= 1,
                   "probability in [0,1] (" + proba[i][c] + ")");
            if (proba[i][c] > proba[i][best]) best = c;
        }
        near(s, 1, 1e-12, "row " + i + " probabilities sum to 1");
        if (best !== pred[i]) disagree++;
    }
    eq(disagree, 0, K + "-class predict === argmax(predictProba)");
    assert((await accuracy(y, pred)) > 0.95, K + "-class separable accuracy (" +
           (await accuracy(y, pred)) + ")");
}
});
__kitS.end();

console.log("test_ml_boosting.js: " + n + " assertions passed");
