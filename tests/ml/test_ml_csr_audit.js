// covers: dyna:ml.CSR
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_csr_audit");
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

await __kitS.section("ctor refuses", async () => {
    for (const bad of [NaN, Infinity]) {
        await throwsAny(() => new CSR([bad, 2], [0, 1], [0, 2], 3),
                  "ctor refuses " + String(bad) + " carried into a CSR");
        await throwsAny(async () => await CSR.fromDense([[1, 2], [bad, 3]]),
                  "fromDense refuses " + String(bad) + " carried into a CSR");
    }
    const Sovf = new CSR([1e308, 1e308], [0, 0], [0, 2], 2);
    await throwsAny(async () => await new LinearRegression().fit(Sovf, [1]),
              "linreg refuses an overflow-class CSR reading");
    await throwsAny(async () => await new LogisticRegression().fit(Sovf, [1]),
              "logreg refuses an overflow-class CSR reading");
    Sovf.close();
    const S = await CSR.fromDense([[1], [2], [3]]);
    await throwsAny(async () => await new LinearRegression().fit(S, [0, NaN, 1]),
              "linreg refuses NaN in y (CSR path)");
    await throwsAny(async () => await new LogisticRegression().fit(S, [0, NaN, 1]),
              "logreg refuses NaN in y (CSR path)");
    const { X, y } = dataset(41, 60, 5);
    const Sclean = await CSR.fromDense(X);
    const a = await new LinearRegression().fit(X, y);
    const b = await new LinearRegression().fit(Sclean, y);
    close(a.intercept, b.intercept, "sparse fit == dense fit (intercept)", 1e-8);
    assert(a.coef.every((v, j) => Math.abs(v - b.coef[j]) <= 1e-8),
           "sparse fit == dense fit (coef)");
    a.close(); b.close(); Sclean.close();
});
__kitS.end();

print("test_ml_audit: all " + n + " tests passed");
