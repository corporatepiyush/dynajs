// covers: dyna:ml.crossValScore
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_cross_val_score_production");
import * as ml from "dyna:ml";

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
                        "\n  want: " + want);
}
async function throws(fn, msg) {
    n++;
    try { await fn(); } catch (e) { return e; }
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
        X.push([c * spread + r() * 2, c * spread + r() * 2, r()]);
        y.push(c);
    }
    return [X, y];
}

await __kitS.section("one score per fold", async () => {
    const [X, y] = blobs(200, 2, 7, 1.2);
    const s = await ml.crossValScore(() => new ml.DecisionTreeClassifier({ maxDepth: 4 }),
                               X, y, { folds: 4 });
    eq(s.length, 4, "one score per fold");
    for (const v of s) assert(v >= 0 && v <= 1, "accuracy in [0,1]");

    eq((await ml.crossValScore(() => new ml.DecisionTreeClassifier(), X, y, { k: 3 })).length, 3,
       "k: 3");
    eq((await ml.crossValScore(() => new ml.DecisionTreeClassifier(), X, y, { folds: 3 })).length, 3,
       "folds: 3 means the same thing");
    eq((await ml.kFold(10, { folds: 3 })).length, (await ml.kFold(10, { k: 3 })).length,
       "and kFold agrees with itself");

    const r2 = await ml.crossValScore(() => new ml.LinearRegression(), X, y,
                                { folds: 3, scoring: async (a, b) => await ml.r2Score(a, b) });
    eq(r2.length, 3, "a custom scorer is used");
    for (const v of r2) assert(v <= 1, "r2 never exceeds 1");

    await throws(async () => await ml.crossValScore(new ml.DecisionTreeClassifier(), X, y),
           "an instance is rejected, a factory is required");
    await throws(async () => await ml.crossValScore(() => ({}), X, y),
           "an object without fit/predict is rejected");
});
__kitS.end();
console.log("test_ml_production.js: " + n + " assertions passed");
