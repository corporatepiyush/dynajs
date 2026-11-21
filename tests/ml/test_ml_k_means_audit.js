// covers: dyna:ml.KMeans
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_k_means_audit");
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

await __kitS.section("KMeans refuses NaN at predict", async () => {
    const { X, y } = dataset(71, 60, 2);
    const Xn = [[NaN, 1]];
    const refusesAtPredict = async (m) => {
        try { await m.predict(Xn); return false; } catch (e) { return true; }
        finally { try { m.close(); } catch (e) { } }
    };
    assert(await refusesAtPredict(await new KMeans(2).fit(X)), "KMeans refuses NaN at predict");
    assert(await refusesAtPredict(await new GaussianNB().fit(X, y)), "GaussianNB refuses NaN at predict");
    assert(await refusesAtPredict(await new KNClassifier(3).fit(X, y)), "KNClassifier refuses NaN at predict");
    assert(await refusesAtPredict(await new SVC().fit(X, y)), "SVC refuses NaN at predict");
    assert(await refusesAtPredict(await new GaussianMixture(2, { seed: 1 }).fit(X)),
           "GaussianMixture refuses NaN at predict");
    {
        const m = await new LinearRegression().fit([[1], [2], [3], [4]], [1, 2, 3, 4]);
        let refused = false;
        try { await m.predict([[NaN]]); } catch (e) { refused = e instanceof RangeError; }
        assert(refused,
               "LinearRegression refuses NaN at predict (gate, API parity with KMeans/GaussianNB/KN/SVC/GMM)");
        m.close();
    }
    {
        const m = await new XGBClassifier({ nEstimators: 5, seed: 1 }).fit(X, y);
        const p = await m.predict(Xn);
        assert(Number.isFinite(p[0]),
               "XGB routes NaN to the missing bin (finite prediction)");
        m.close();
    }
});
__kitS.end();

print("test_ml_audit: all " + n + " tests passed");
