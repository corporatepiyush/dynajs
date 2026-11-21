// covers: dyna:ml.averagePrecision
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_average_precision_audit");
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

await __kitS.section("AP tie group is one threshold", async () => {
    close(await averagePrecision([1, 0, 1], [0.9, 0.9, 0.1]), 7 / 12,
          "AP tie group is one threshold");
    close(await averagePrecision([1, 1, 0], [0.5, 0.5, 0.5]), 2 / 3,
          "AP all-tied = base rate");
    close(await averagePrecision([1, 1, 0, 0], [0.9, 0.9, 0.1, 0.1]), 1.0,
          "AP pure tie runs");
    close(await averagePrecision([0, 0, 1, 1], [0.1, 0.2, 0.8, 0.9]), 1.0,
          "AP separable unchanged");
    close(await averagePrecision([1, 0, 0], [0.9, 0.5, 0.5]), 1.0,
          "AP negative-only tie group adds nothing");
});
__kitS.end();

print("test_ml_audit: all " + n + " tests passed");
