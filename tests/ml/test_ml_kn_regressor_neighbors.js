// covers: dyna:ml.KNRegressor
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_kn_regressor_neighbors");
import { KNClassifier, KNRegressor, DBScan } from "dyna:ml";

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
function refKnn(Xtrain, ytrain, k, weighted, regressor, q) {
    const d = Xtrain.map((row, i) => {
        let s = 0;
        for (let j = 0; j < row.length; j++) { const t = row[j] - q[j]; s += t * t; }
        return { d: s, y: ytrain[i], i };
    });
    d.sort((a, b) => (a.d - b.d) || (a.i - b.i));
    const near = d.slice(0, k);
    if (regressor) {
        if (weighted) {
            const hit = near.find((e) => e.d === 0);
            if (hit) return hit.y;
            let num = 0, den = 0;
            for (const e of near) { const w = 1 / Math.sqrt(e.d); num += w * e.y; den += w; }
            return num / den;
        }
        return near.reduce((s, e) => s + e.y, 0) / k;
    }
    const score = new Map();
    for (const e of near) {
        const w = weighted ? (e.d === 0 ? Infinity : 1 / Math.sqrt(e.d)) : 1;
        score.set(e.y, (score.get(e.y) || 0) + w);
    }
    let bestLab = null, bestScore = -1;
    for (const [lab, sc] of score)
        if (sc > bestScore || (sc === bestScore && lab < bestLab)) { bestScore = sc; bestLab = lab; }
    return bestLab;
}

await __kitS.section("mean of the two nearest", async () => {
    const X = [[0], [1], [2], [3]];
    const y = [10, 20, 30, 40];
    const m = new KNRegressor(2);
    try {
        await m.fit(X, y);
        approx((await m.predict([[0.5]]))[0], 15, 1e-12, "mean of the two nearest");
        approx((await m.predict([[2.4]]))[0], 35, 1e-12, "mean of 30 and 40");
    } finally { m.close(); }
});

await __kitS.section("exact hit returns its own target exactly", async () => {
    const m = new KNRegressor(3, "distance");
    try {
        await m.fit([[0], [1], [2]], [5, 100, 200]);
        approx((await m.predict([[0]]))[0], 5, 0, "exact hit returns its own target exactly");
        const mid = (await m.predict([[0.5]]))[0];
        assert(mid > 5 && mid < 200, "a between-point blends its neighbours");
    } finally { m.close(); }
});

await __kitS.section("reentrant close during fit", async () => {
    const m = new KNRegressor(1);
    await throws(async () => await m.fit([[{ valueOf() { m.close(); return 1; } }], [2]], [0, 1]),
        "reentrant close during fit");
    assert(m.closed === true, "closed");
});
__kitS.end();

print("test_ml_neighbors: all tests passed (" + n + " assertions)");
