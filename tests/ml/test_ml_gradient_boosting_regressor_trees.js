// covers: dyna:ml.GradientBoostingRegressor
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_gradient_boosting_regressor_trees");
import { DecisionTreeClassifier, DecisionTreeRegressor, RandomForestClassifier,
         RandomForestRegressor, GradientBoostingRegressor,
         accuracy, meanSquaredError } from "dyna:ml";

let n = 0;
function assert(cond, msg) { n++; if (!cond) throw new Error("assertion failed: " + msg); }
function approx(a, b, eps, msg) {
    n++;
    if (!(Math.abs(a - b) <= eps)) throw new Error(`approx failed: ${msg} (${a} vs ${b})`);
}
async function throws(fn, msg, ErrType) {
    n++;
    try { await fn(); } catch (e) {
        if (ErrType && !(e instanceof ErrType))
            throw new Error(`${msg}: wrong error type ${e.constructor.name}: ${e.message}`);
        return;
    }
    throw new Error("expected a throw: " + msg);
}
function rng(seed) {
    let s = seed >>> 0;
    return () => { s = (s * 1664525 + 1013904223) >>> 0; return s / 4294967296; };
}
function xorData(perQuadrant, seed) {
    const rnd = rng(seed);
    const X = [], y = [];
    for (const [cx, cy, lab] of [[0, 0, 0], [1, 1, 0], [0, 1, 1], [1, 0, 1]])
        for (let i = 0; i < perQuadrant; i++) {
            X.push([cx + (rnd() - 0.5) * 0.3, cy + (rnd() - 0.5) * 0.3]);
            y.push(lab);
        }
    return { X, y };
}

await __kitS.section("120 rounds fit x^2 closely:", async () => {
    const rnd = rng(6161);
    const X = [], y = [];
    for (let i = 0; i < 80; i++) {
        const x = rnd() * 6 - 3;
        X.push([x]);
        y.push(x * x);
    }
    const one = new GradientBoostingRegressor({ nEstimators: 1, maxDepth: 2 });
    const many = new GradientBoostingRegressor({ nEstimators: 120, maxDepth: 2 });
    try {
        await one.fit(X, y); await many.fit(X, y);
        const mse1 = await meanSquaredError(y, await one.predict(X));
        const mseN = await meanSquaredError(y, await many.predict(X));
        assert(mseN < mse1, `boosting reduces error with more rounds (${mseN} < ${mse1})`);
        assert(mseN < 0.5, "120 rounds fit x^2 closely: " + mseN);
        const flat = new GradientBoostingRegressor({ nEstimators: 5 });
        try {
            await flat.fit([[1], [1], [1]], [4, 6, 8]);
            approx((await flat.predict([[1]]))[0], 6, 1e-9,
                   "a constant feature leaves only the base mean");
        } finally { flat.close(); }
    } finally { one.close(); many.close(); }
});

await __kitS.section("learningRate 0.001 barely moves off the base", async () => {
    const X = [], y = [];
    for (let i = 0; i < 20; i++) { X.push([i]); y.push(i < 10 ? 0 : 100); }
    const slow = new GradientBoostingRegressor({ nEstimators: 2, learningRate: 0.001, maxDepth: 1 });
    const fast = new GradientBoostingRegressor({ nEstimators: 2, learningRate: 1.0, maxDepth: 1 });
    try {
        await slow.fit(X, y); await fast.fit(X, y);
        const base = 50;
        assert(Math.abs((await slow.predict([[19]]))[0] - base) < 1,
               "learningRate 0.001 barely moves off the base");
        assert(Math.abs((await fast.predict([[19]]))[0] - 100) < 1,
               "learningRate 1.0 reaches the target in one step");
    } finally { slow.close(); fast.close(); }
});

await __kitS.section("block L76", async () => {
    const rnd = rng(4242);
    const X = [], y = [];
    for (let i = 0; i < 50; i++) { const x = rnd() * 4; X.push([x]); y.push(Math.sin(x)); }
    const a = new GradientBoostingRegressor({ nEstimators: 20, subsample: 0.6, seed: 21, maxDepth: 2 });
    const b = new GradientBoostingRegressor({ nEstimators: 20, subsample: 0.6, seed: 21, maxDepth: 2 });
    try {
        await a.fit(X, y); await b.fit(X, y);
        const pa = await a.predict(X), pb = await b.predict(X);
        for (let i = 0; i < pa.length; i++)
            approx(pa[i], pb[i], 0, `subsampled fit is seed-reproducible @${i}`);
    } finally { a.close(); b.close(); }
});

await __kitS.section("reentrant close during boosting fit", async () => {
    const m = new GradientBoostingRegressor({ nEstimators: 3 });
    await throws(async () => await m.fit([[{ valueOf() { m.close(); return 1; } }], [2]], [0, 1]),
        "reentrant close during boosting fit");
    assert(m.closed === true, "closed");
});
__kitS.end();

print("test_ml_trees: all tests passed (" + n + " assertions)");
