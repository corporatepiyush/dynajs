// covers: dyna:ml.LogisticRegression
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_logistic_regression_audit");
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

await __kitS.section("empty record refused", async () => {
    const m = new LogisticRegression();
    await m.fit([[1, 2], [2, 1], [5, 5], [6, 4], [3, 3], [4, 6]], [0, 0, 1, 1, 0, 1]);
    const bytes = m.serialize();
    m.close();
    await throwsAny(() => LogisticRegression.deserialize(new Uint8Array(0)),
              "empty record refused");
    await throwsAny(() => LogisticRegression.deserialize(bytes.slice(0, 9)),
              "truncated header refused");
    for (let cut = 1; cut < bytes.length; cut += 7) {
        const t = bytes.slice(0, bytes.length - cut);
        let refused = false;
        try { LogisticRegression.deserialize(t); } catch (e) { refused = true; }
        if (!refused) throw new Error("truncated record (" + cut + " bytes cut) accepted");
    }
    n++;
    let loaded = 0;
    for (let i = 0; i < bytes.length; i++) {
        for (const bit of [0x01, 0x80]) {
            const f = bytes.slice();
            f[i] ^= bit;
            try {
                const mm = LogisticRegression.deserialize(f);
                await mm.predict([[1, 2]]);
                mm.close();
                loaded++;
            } catch (e) {  }
        }
    }
    assert(loaded <= 2 * bytes.length, "bit-flip sweep terminated");
    const k = await new KMeans(2, 1).fit([[1], [2], [3]]);
    const kb = k.serialize();
    k.close();
    await throwsAny(() => LogisticRegression.deserialize(kb), "cross-class record refused");
});

await __kitS.section("predict after close throws", async () => {
    const m = new LogisticRegression();
    await m.fit([[1, 2], [2, 1], [5, 5], [6, 4]], [0, 0, 1, 1]);
    m.close();
    m.close();
    n++;
    await throwsAny(async () => await m.predict([[1, 2]]), "predict after close throws");
    await throwsAny(() => m.coef, "getter after close throws");
    const scaler = new StandardScaler();
    const clf = new DecisionTreeClassifier({ maxDepth: 2 });
    const p = new Pipeline([scaler, clf]);
    const X = [[1, 2], [2, 1], [5, 5], [6, 4]], y = [0, 0, 1, 1];
    await p.fit(X, y);
    await p.predict(X);
    p.close();
    p.close();
    n++;
    await throwsAny(async () => await p.predict(X), "pipeline predict after close throws");
    const t = await scaler.transform(X);
    assert(t.length === 4, "pipeline close leaves stages open (by-reference ownership)");
    scaler.close();
    clf.close();
});
__kitS.end();

print("test_ml_audit: all " + n + " tests passed");
