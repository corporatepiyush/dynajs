// covers: dyna:ml.accuracy
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_accuracy_trees");
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

await __kitS.section("a different seed still predicts every row", async () => {
    const { X, y } = xorData(25, 31337);
    const a = new RandomForestClassifier({ nEstimators: 15, seed: 7 });
    const b = new RandomForestClassifier({ nEstimators: 15, seed: 7 });
    const c = new RandomForestClassifier({ nEstimators: 15, seed: 8 });
    try {
        await a.fit(X, y); await b.fit(X, y); await c.fit(X, y);
        const pa = await a.predict(X), pb = await b.predict(X), pc = await c.predict(X);
        for (let i = 0; i < X.length; i++)
            assert(pa[i] === pb[i], `same seed -> same prediction @${i}`);
        assert(pc.length === pa.length, "a different seed still predicts every row");
        assert((await accuracy(y, pa)) > 0.9, "forest fits XOR: " + (await accuracy(y, pa)));
        assert((await accuracy(y, pc)) > 0.9, "…with the other seed too: " + (await accuracy(y, pc)));
    } finally { a.close(); b.close(); c.close(); }
});
__kitS.end();

print("test_ml_trees: all tests passed (" + n + " assertions)");
