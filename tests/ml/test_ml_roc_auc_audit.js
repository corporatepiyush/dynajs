// covers: dyna:ml.rocAuc
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_roc_auc_audit");
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

await __kitS.section("confusionMatrix label cap is 4095", async () => {
    await confusionMatrix([4095], [4095]);
    n++;
    await throwsRange(async () => await confusionMatrix([4096], [0]), "confusionMatrix label cap is 4095");
    close(await rocAuc([1, 0, 1, 0], [5, 5, 5, 5]), 0.5, "rocAuc all-tied is exactly 0.5");
    close(await rocAuc([1, 1, 0, 0, 0], [3, 5, 1, 3, 4]), 0.75, "rocAuc tie-averaged ranks");
    for (const [N, k] of [[10, 3], [7, 7], [9, 4], [33, 5]]) {
        const folds = await kFold(N, { k, shuffle: true, seed: 8 });
        const inTest = new Array(N).fill(0);
        for (const f of folds) {
            const mark = new Array(N).fill(0);
            for (const i of f.test) { inTest[i]++; mark[i]++; }
            for (const i of f.train) mark[i]++;
            if (!mark.every(c => c === 1))
                throw new Error("kFold(" + N + "," + k + "): train is not the complement");
        }
        if (!inTest.every(c => c === 1))
            throw new Error("kFold(" + N + "," + k + "): test folds do not cover exactly once");
        n++;
    }
});
__kitS.end();

print("test_ml_audit: all " + n + " tests passed");
