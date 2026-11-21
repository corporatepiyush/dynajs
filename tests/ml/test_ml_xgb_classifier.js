// covers: dyna:ml.XGBClassifier
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_xgb_classifier");
import { XGBRegressor, XGBClassifier, GradientBoostingRegressor, accuracy, r2Score } from "dyna:ml";

let n = 0;
function assert(c, m) { n++; if (!c) throw new Error("assertion failed: " + m); }
function close(a, b, eps, m) {
    n++;
    if (!(Math.abs(a - b) <= eps))
        throw new Error((m || "not close") + ": " + a + " vs " + b);
}
async function throws(fn, kind, m) {
    n++;
    try { await fn(); } catch (e) {
        if (kind && !(e instanceof kind)) throw new Error((m || "wrong error") + ": " + e);
        return String(e);
    }
    throw new Error((m || "expected a throw") + " but none happened");
}

function lcg(seed) { let s = seed >>> 0; return () => (s = (s * 1664525 + 1013904223) >>> 0) / 4294967296; }

function linearData(seed, rows, noise) {
    const rnd = lcg(seed), X = [], y = [], yc = [];
    for (let i = 0; i < rows; i++) {
        const a = rnd() * 4 - 2, b = rnd() * 4 - 2, c = rnd() * 4 - 2;
        const s = 2 * a - b + 0.5 * c;
        X.push([a, b, c]);
        y.push(s + (rnd() * 2 - 1) * noise);
        yc.push(1 / (1 + Math.exp(-s)) > rnd() ? 1 : 0);
    }
    return { X, y, yc };
}

await __kitS.section("three-class fit", async () => {
    const rnd = lcg(77), X = [], y = [];
    for (let i = 0; i < 400; i++) {
        const a = rnd() * 4 - 2, b = rnd() * 4 - 2;
        X.push([a, b]);
        y.push(a > 0 ? (b > 0 ? 2 : 1) : 0);
    }
    const m = await new XGBClassifier({ nEstimators: 40, maxDepth: 3 }).fit(X, y);
    assert((await accuracy(y, await m.predict(X))) > 0.95, "three-class fit");
    const P = await m.predictProba(X);
    assert(P[0].length === 3, "one column per class");
    for (let i = 0; i < 20; i++) {
        let s = 0;
        for (const v of P[i]) { s += v; assert(v >= 0 && v <= 1, "probability in range"); }
        close(s, 1, 1e-12, "multiclass row sums to 1");
    }
    const lab = await m.predict(X);
    for (let i = 0; i < lab.length; i++) {
        let best = 0;
        for (let k = 1; k < 3; k++) if (P[i][k] > P[i][best]) best = k;
        if (lab[i] !== best) assert(false, "multiclass predict === argmax at row " + i);
    }
    assert((await m.apply([X[0]]))[0].length === 40 * 3, "apply width is rounds x classes");

    await throws(async () => await new XGBClassifier().fit([[1], [2]], [5, 5]), TypeError,
           "a booster cannot fit a single-label target");
});
__kitS.end();

print("test_ml_xgb: all " + n + " assertions passed");
