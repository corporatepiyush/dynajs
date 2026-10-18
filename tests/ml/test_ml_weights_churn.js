// flags: --std
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
{
    const S = await ml.CSR.fromDense(X);
    for (let i = 0; i < 100; i++)
        await new LinearRegression().fit(S, y, { sampleWeight: w });
}

for (let i = 0; i < 200; i++) {
    await throwsKind(async () => await new LinearRegression().fit([[1], [2], [NaN]], [1, 2, 3],
                                                { sampleWeight: [1, 1, 1] }),
               RangeError, "NaN fit with weights refuses");
    await throwsKind(async () => await new LogisticRegression().fit([[1], [2], [Infinity]], [0, 1, 1],
                                                  { sampleWeight: [1, 1, 1] }),
               RangeError, "infinite fit with weights refuses");
    await throwsKind(async () => await new StandardScaler().fit([[1], [2]], { sampleWeight: [1, -1] }),
               RangeError, "negative weight refuses");
    await throwsKind(async () => await new StandardScaler().fit([[1], [2]], { sampleWeight: [0, 0] }),
               RangeError, "all-zero weights refuse");
    await throwsKind(async () => await new PCA(1).fit(X, { sampleWeight: w }),
               TypeError, "an unweighted estimator refuses sampleWeight");
    await throwsKind(async () => await new KNRegressor(2).fit(X, y, { sampleWeight: w }),
               TypeError, "KNRegressor refuses sampleWeight");
}

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
{
    const path = `${std.getenv("TMPDIR") || "/tmp"}/dj_ml_w_${Date.now()}.bin`;
    const m = await new LinearRegression().fit(X, y, { sampleWeight: w });
    await m.save(new Path(path));
    const loaded = await ml.LinearRegression.load(new Path(path));
    assert(JSON.stringify(await loaded.predict(X)) === JSON.stringify(await m.predict(X)),
        "save/load is prediction-identical");
    loaded.close();
    m.close();
    file.remove(new Path(path));
}

{
    const a = await new LinearRegression().fit(X, y);
    const b = await new LinearRegression().fit(X, y, { sampleWeight: [1, 1, 1, 1, 1, 1] });
    assert(a.coef.every((v, i) => Object.is(v, b.coef[i])) &&
           Object.is(a.intercept, b.intercept),
        "all-ones weights remain bit-identical to no weights");
}

print("test_ml_weights_churn: all " + n + " assertions passed");
