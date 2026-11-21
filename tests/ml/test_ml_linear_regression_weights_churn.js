// flags: --std
// covers: dyna:ml.LinearRegression
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_linear_regression_weights_churn");
import * as ml from "dyna:ml";
import * as std from "std";
import * as file from "dyna:file";
import { Path } from "dyna:file";
import {
    LinearRegression, LogisticRegression, GaussianNB, StandardScaler,
    PCA, KNRegressor, DecisionTreeRegressor,
} from "dyna:ml";

let n = 0;
function assert(c, msg) { n++; if (!c) throw new Error("assertion failed: " + msg); }
async function throwsKind(fn, kind, msg) {
    n++;
    try { await fn(); } catch (e) {
        if (kind && !(e instanceof kind))
            throw new Error(msg + ": wrong error type: " + e);
        return;
    }
    throw new Error(msg + ": expected a throw");
}

const X = [[1], [2], [3], [4], [5], [6]];
const y = [1, 2, 3, 4, 8, 12];
const yc = [0, 0, 0, 1, 1, 1];
const w = [1, 1, 2, 1, 0.5, 2];

for (let i = 0; i < 400; i++) {
    const a = await new LinearRegression().fit(X, y, { sampleWeight: w });
    const b = await new LogisticRegression({ maxIter: 50 }).fit(X, yc, { sampleWeight: w });
    const c = await new GaussianNB().fit(X, yc, { sampleWeight: w });
    const d = await new StandardScaler().fit(X, { sampleWeight: w });
    const e = await new DecisionTreeRegressor({ maxDepth: 2 }).fit(X, y, { sampleWeight: w });
    if (i === 0) {
        assert(Array.isArray(a.coef) && a.coef.length === 1, "linreg fitted");
        assert(Array.isArray(await b.predict([[3.5]])), "logreg fitted");
        assert(Array.isArray(await c.predict([[3.5]])), "nb fitted");
        assert(Array.isArray(await d.transform(X)), "scaler fitted");
        assert(Array.isArray(await e.predict([[3.5]])), "tree fitted");
    }
}

await __kitS.section("save/load is prediction-identical", async () => {
    const path = `${std.getenv("TMPDIR") || "/tmp"}/dj_ml_w_${Date.now()}_${Math.random().toString(36).slice(2, 10)}.bin`;
    const m = await new LinearRegression().fit(X, y, { sampleWeight: w });
    await m.save(new Path(path));
    const loaded = await ml.LinearRegression.load(new Path(path));
    assert(JSON.stringify(await loaded.predict(X)) === JSON.stringify(await m.predict(X)),
        "save/load is prediction-identical");
    loaded.close();
    m.close();
    file.remove(new Path(path));
});

await __kitS.section("all-ones weights remain bit-identical to no weights", async () => {
    const a = await new LinearRegression().fit(X, y);
    const b = await new LinearRegression().fit(X, y, { sampleWeight: [1, 1, 1, 1, 1, 1] });
    assert(a.coef.every((v, i) => Object.is(v, b.coef[i])) &&
           Object.is(a.intercept, b.intercept),
        "all-ones weights remain bit-identical to no weights");
});
__kitS.end();

print("test_ml_weights_churn: all " + n + " assertions passed");
