// covers: dyna:ml.meanSquaredError
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_mean_squared_error_trees");
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

await __kitS.section("block L36", async () => {
    const rnd = rng(808);
    const X = [], y = [];
    for (let i = 0; i < 60; i++) {
        const x = rnd() * 10;
        X.push([x, rnd()]);
        y.push(3 * x + 2 + (rnd() - 0.5) * 0.5);
    }
    const m = new RandomForestRegressor({ nEstimators: 25, seed: 3 });
    try {
        await m.fit(X, y);
        const p = await m.predict(X);
        const lo = Math.min(...y), hi = Math.max(...y);
        for (let i = 0; i < p.length; i++)
            assert(p[i] >= lo - 1e-9 && p[i] <= hi + 1e-9,
                   `averaged prediction stays in the target range @${i}`);
        const mean = y.reduce((s, v) => s + v, 0) / y.length;
        const mseModel = await meanSquaredError(y, p);
        const mseMean = await meanSquaredError(y, y.map(() => mean));
        assert(mseModel < mseMean * 0.2,
               `forest beats the mean baseline (${mseModel} vs ${mseMean})`);
    } finally { m.close(); }
});
__kitS.end();

print("test_ml_trees: all tests passed (" + n + " assertions)");
