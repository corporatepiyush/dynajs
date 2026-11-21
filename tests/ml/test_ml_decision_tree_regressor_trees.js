// covers: dyna:ml.DecisionTreeRegressor
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_decision_tree_regressor_trees");
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

await __kitS.section("left level", async () => {
    const X = [], y = [];
    for (let i = 0; i < 20; i++) { X.push([i]); y.push(i < 10 ? 5 : 20); }
    const m = new DecisionTreeRegressor({ maxDepth: 1 });
    try {
        await m.fit(X, y);
        const p = await m.predict([[0], [9], [10], [19]]);
        approx(p[0], 5, 1e-12, "left level");
        approx(p[1], 5, 1e-12, "still left at the boundary");
        approx(p[2], 20, 1e-12, "right level");
        approx(p[3], 20, 1e-12, "right level at the end");
        approx(await meanSquaredError(y, await m.predict(X)), 0, 1e-12,
               "a step is fitted with zero error by one split");
    } finally { m.close(); }
});

await __kitS.section("mean of 1,2,3", async () => {
    const m = new DecisionTreeRegressor({ maxDepth: 1 });
    try {
        await m.fit([[0], [0], [0], [10]], [1, 2, 3, 100]);
        approx((await m.predict([[0]]))[0], 2, 1e-12, "mean of 1,2,3");
        approx((await m.predict([[10]]))[0], 100, 1e-12, "the lone right sample");
    } finally { m.close(); }
});
__kitS.end();

print("test_ml_trees: all tests passed (" + n + " assertions)");
