// covers: dyna:ml.GaussianMixture
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_gaussian_mixture_audit");
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

await __kitS.section("gmm logLikelihood is a finite positive total (blobs data)", async () => {
    const r = lcg(202), X = [];
    for (let i = 0; i < 90; i++) {
        const c = i % 3;
        X.push([[0, 6, 3][c] + (r() - 0.5) * 0.4, [0, 0, 5][c] + (r() - 0.5) * 0.4]);
    }
    const g = await new GaussianMixture(3, { seed: 42, maxIter: 500 }).fit(X);
    assert(isFinite(g.logLikelihood) && g.logLikelihood > 0,
           "gmm logLikelihood is a finite positive total (blobs data)");
    g.close();
});
__kitS.end();

print("test_ml_audit: all " + n + " tests passed");
