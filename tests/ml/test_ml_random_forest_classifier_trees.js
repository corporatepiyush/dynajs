// covers: dyna:ml.RandomForestClassifier
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_random_forest_classifier_trees");
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

await __kitS.section("40 trees fit well:", async () => {
    const { X, y } = xorData(30, 5150);
    const few = new RandomForestClassifier({ nEstimators: 3, seed: 11 });
    const many = new RandomForestClassifier({ nEstimators: 40, seed: 11 });
    try {
        await few.fit(X, y); await many.fit(X, y);
        const accFew = await accuracy(y, await few.predict(X));
        const accMany = await accuracy(y, await many.predict(X));
        assert(accMany >= accFew - 0.05,
               `40 trees are not worse than 3 (${accMany} vs ${accFew})`);
        assert(accMany > 0.9, "40 trees fit well: " + accMany);
    } finally { few.close(); many.close(); }
});

await __kitS.section("maxFeatures=1 still learns XOR by ensembling", async () => {
    const { X, y } = xorData(20, 1212);
    const m = new RandomForestClassifier({ nEstimators: 30, maxFeatures: 1, seed: 5 });
    try {
        await m.fit(X, y);
        assert((await accuracy(y, await m.predict(X))) > 0.85,
               "maxFeatures=1 still learns XOR by ensembling");
    } finally { m.close(); }
});

await __kitS.section("reentrant close during forest predict", async () => {
    const m = new RandomForestClassifier({ nEstimators: 3 });
    await m.fit([[1], [2], [3], [4]], [0, 0, 1, 1]);
    await throws(async () => await m.predict([[{ valueOf() { m.close(); return 1; } }]]),
        "reentrant close during forest predict");
    assert(m.closed === true, "closed");
});
__kitS.end();

print("test_ml_trees: all tests passed (" + n + " assertions)");
