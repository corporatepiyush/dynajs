// flags: --std
// covers: dyna:ml.LogisticRegression
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_logistic_regression_weights_churn");
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

await __kitS.section("deserialize is prediction-identical", async () => {
for (let i = 0; i < 100; i++) {
    const m = await new LogisticRegression({ maxIter: 20 }).fit(X, yc, { sampleWeight: w });
    const bytes = m.serialize();
    const back = ml.LogisticRegression.deserialize(bytes);
    assert(JSON.stringify(await back.predict(X)) === JSON.stringify(await m.predict(X)),
        "deserialize is prediction-identical");
    back.close();
    m.close();
    m.close();
}
});
__kitS.end();

print("test_ml_weights_churn: all " + n + " assertions passed");
