// covers: dyna:ml.crossValScore
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_cross_val_score_audit");
import {
    averagePrecision, stratifiedKFold, kFold, confusionMatrix, rocAuc,
    crossValScore, gridSearch,
    LinearRegression, LogisticRegression, KMeans, SVC, GaussianMixture,
    GaussianNB, DecisionTreeClassifier, DecisionTreeRegressor,
    RandomForestClassifier, RandomForestRegressor, GradientBoostingClassifier,
    GradientBoostingRegressor, XGBClassifier, XGBRegressor, PCA, KNClassifier,
    KNRegressor, DBScan, StandardScaler, MinMaxScaler, CSR, Pipeline,
} from "dyna:ml";

let n = 0;
function assert(c, m) { n++; if (!c) throw new Error("assertion failed: " + m); }
function close(got, want, m, tol = 1e-12) {
    n++;
    if (!(Math.abs(got - want) <= tol))
        throw new Error("assertion failed: " + m + "\n  got:  " + got + "\n  want: " + want);
}
async function throwsWhich(fn, ctor, m) {
    n++;
    try { await fn(); } catch (e) {
        if (e instanceof ctor) return;
        throw new Error("assertion failed (wrong error): " + m + " -> " + e);
    }
    throw new Error("assertion failed (expected " + ctor.name + "): " + m);
}
const throwsRange = async (fn, m) => await throwsWhich(fn, RangeError, m);
const throwsAny = async (fn, m) => await throwsWhich(fn, Error, m);
function lcg(seed) {
    let s = seed >>> 0;
    return () => { s = (s * 1664525 + 1013904223) >>> 0; return s / 4294967296; };
}
function dataset(seed, rows, cols) {
    const r = lcg(seed), X = [], y = [];
    for (let i = 0; i < rows; i++) {
        const row = [];
        for (let j = 0; j < cols; j++) row.push(r() * 4 - 2);
        X.push(row);
        y.push(row[0] + row[1] > 0 ? 1 : 0);
    }
    return { X, y };
}

await __kitS.section("crossValScore returned k scores", async () => {
    const X = [], y = [];
    const r = lcg(81);
    for (let i = 0; i < 60; i++) {
        X.push([r() * 4 - 2, r() * 4 - 2]);
        y.push(X[i][0] > 0 ? 1 : 0);
    }
    let open = 0;
    const factory = () => {
        const m = new LogisticRegression();
        open++;
        const origClose = m.close.bind(m);
        m.close = () => { if (open > 0) open--; origClose(); };
        return m;
    };
    const scores = await crossValScore(factory, X, y, { k: 5 });
    assert(scores.length === 5, "crossValScore returned k scores");
    assert(open === 0, "crossValScore closed every fold estimator");
    const gs = await gridSearch(factory, X, y, { maxIter: [50, 100] }, { k: 3 });
    assert(gs.results.length === 2 && gs.best && typeof gs.bestScore === "number",
           "gridSearch structure");
    assert(open === 0, "gridSearch closed every (point, fold) estimator");
});
__kitS.end();

print("test_ml_audit: all " + n + " tests passed");
