// covers: dyna:ml.XGBClassifier
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_xgb_classifier_audit");
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

await __kitS.section("seed", async () => {
    // A split that sends ONLY the missing values left is stored with a -Infinity
    // threshold. The loader refused every non-finite threshold, so about one XGB
    // model in ten trained on data with missing values saved and then could not
    // be loaded (measured: 31/300 classifiers, 15/300 regressors). Which seeds
    // hit it depends on rounding, so sweep enough of them that some always do.
    let roundTrips = 0;
    for (let seed = 1; seed <= 60; seed++) {
        const r = lcg(seed), Xm = [], yc = [], yv = [];
        const rowsM = 20 + (seed % 5) * 10;
        for (let i = 0; i < rowsM; i++) {
            const c = r() < 0.5 ? 1 : 0;
            Xm.push([(c ? r() < 0.8 : r() < 0.1) ? NaN : r(), r(), r() < 0.3 ? NaN : r()]);
            yc.push(c);
            yv.push(c + r() * 0.3);
        }
        for (const [name, make] of [
            ["XGBClassifier", async () => await new XGBClassifier({ nEstimators: 3, seed: 1 }).fit(Xm, yc)],
            ["XGBRegressor", async () => await new XGBRegressor({ nEstimators: 3, seed: 1 }).fit(Xm, yv)],
        ]) {
            let m;
            try { m = await make(); } catch (e) { continue; }
            const before = JSON.stringify(await m.predict(Xm));
            let loaded = null, why = "";
            try { loaded = m.constructor.deserialize(m.serialize()); } catch (e) { why = String(e.message); }
            if (loaded === null)
                throw new Error(name + " seed " + seed + ": a model fitted on missing values does not load back (" + why + ")");
            if (JSON.stringify(await loaded.predict(Xm)) !== before)
                throw new Error(name + " seed " + seed + ": the reloaded model predicts differently");
            loaded.close();
            m.close();
            roundTrips++;
        }
    }
    assert(roundTrips >= 100, "missing-value XGB models round-trip (" + roundTrips + " checked)");
});
__kitS.end();

print("test_ml_audit: all " + n + " tests passed");
