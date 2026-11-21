// covers: dyna:ml.RandomForestRegressor
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_random_forest_regressor_trees");
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

await __kitS.section("predict before fit", async () => {
    const m = new RandomForestRegressor({ nEstimators: 2 });
    try {
        await throws(async () => await m.predict([[1]]), "predict before fit");
        assert(m.depth === 0, "depth is 0 before fit");
        await m.fit([[1], [2], [3]], [1, 2, 3]);
        await throws(async () => await m.predict([[1, 2]]), "wrong feature count", TypeError);
        await throws(async () => await m.fit([[1], [2]], [1]), "y length mismatch", TypeError);
    } finally { m.close(); }
});
__kitS.end();

print("test_ml_trees: all tests passed (" + n + " assertions)");
