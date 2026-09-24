// flags: --std
/* test_ml_weights_churn.js -- the {sampleWeight} lifecycle, churned.
 *
 * The defect this pins: the weights working buffer (an owned double[] every
 * weighted fit ingests) was retained on some exits -- the LogisticRegression
 * DENSE success path (the leak the ASan battery reported: one buffer per
 * weighted fit, for the process lifetime), the non-finite refusal path of the
 * shared ingest (after the weights were already ingested), and the
 * weights-refusal path of the X-only ingest (which leaked the matrix too).
 *
 * Every row below is a path that leaked BEFORE; the gate is LSan-flat over
 * the churn (run this file under the ASan build with detect_leaks=1). The
 * behavioural assertions pin that the fix did not change any answer.
 *
 * Also churned: serialize/deserialize and save/load around weighted fits
 * (the model-lifecycle surface the leak was reported under).
 *
 * Run: dynajs --std tests/test_ml_weights_churn.js
 */
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
function throwsKind(fn, kind, msg) {
    n++;
    try { fn(); } catch (e) {
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

/* ---- 1. the success-path churn: fit x N across every weighted estimator.
   The LinearRegression/LogisticRegression dense paths were the leaks. ---- */
for (let i = 0; i < 400; i++) {
    const a = new LinearRegression().fit(X, y, { sampleWeight: w });
    const b = new LogisticRegression({ maxIter: 50 }).fit(X, yc, { sampleWeight: w });
    const c = new GaussianNB().fit(X, yc, { sampleWeight: w });
    const d = new StandardScaler().fit(X, { sampleWeight: w });
    const e = new DecisionTreeRegressor({ maxDepth: 2 }).fit(X, y, { sampleWeight: w });
    if (i === 0) {
        assert(Array.isArray(a.coef) && a.coef.length === 1, "linreg fitted");
        assert(Array.isArray(b.predict([[3.5]])), "logreg fitted");
        assert(Array.isArray(c.predict([[3.5]])), "nb fitted");
        assert(Array.isArray(d.transform(X)), "scaler fitted");
        assert(Array.isArray(e.predict([[3.5]])), "tree fitted");
    }
}
/* the sparse LinearRegression fit takes the weights too */
{
    const S = ml.CSR.fromDense(X);
    for (let i = 0; i < 100; i++)
        new LinearRegression().fit(S, y, { sampleWeight: w });
}

/* ---- 2. the refusal paths that leaked (each throws AND frees) ---- */
for (let i = 0; i < 200; i++) {
    /* non-finite input AFTER the weights were ingested: the shared ingest's
       check_finite refusal freed y but kept w */
    throwsKind(() => new LinearRegression().fit([[1], [2], [NaN]], [1, 2, 3],
                                                { sampleWeight: [1, 1, 1] }),
               RangeError, "NaN fit with weights refuses");
    throwsKind(() => new LogisticRegression().fit([[1], [2], [Infinity]], [0, 1, 1],
                                                  { sampleWeight: [1, 1, 1] }),
               RangeError, "infinite fit with weights refuses");
    /* a bad weight itself: the X-only ingest leaked the matrix here */
    throwsKind(() => new StandardScaler().fit([[1], [2]], { sampleWeight: [1, -1] }),
               RangeError, "negative weight refuses");
    throwsKind(() => new StandardScaler().fit([[1], [2]], { sampleWeight: [0, 0] }),
               RangeError, "all-zero weights refuse");
    /* an estimator with NO weighted fit refuses rather than ignores */
    throwsKind(() => new PCA(1).fit(X, { sampleWeight: w }),
               TypeError, "an unweighted estimator refuses sampleWeight");
    throwsKind(() => new KNRegressor(2).fit(X, y, { sampleWeight: w }),
               TypeError, "KNRegressor refuses sampleWeight");
}

/* ---- 3. serialize/close lifecycle churn around weighted fits ---- */
for (let i = 0; i < 100; i++) {
    const m = new LogisticRegression({ maxIter: 20 }).fit(X, yc, { sampleWeight: w });
    const bytes = m.serialize();
    const back = ml.LogisticRegression.deserialize(bytes);
    assert(JSON.stringify(back.predict(X)) === JSON.stringify(m.predict(X)),
        "deserialize is prediction-identical");
    back.close();
    m.close();
    m.close();          /* idempotent */
}
{
    /* save/load across processes-of-truth: the file path, not the bytes */
    const path = `${std.getenv("TMPDIR") || "/tmp"}/dj_ml_w_${Date.now()}.bin`;
    const m = new LinearRegression().fit(X, y, { sampleWeight: w });
    m.save(new Path(path));
    const loaded = ml.LinearRegression.load(new Path(path));
    assert(JSON.stringify(loaded.predict(X)) === JSON.stringify(m.predict(X)),
        "save/load is prediction-identical");
    loaded.close();
    m.close();
    file.remove(new Path(path));
}

/* ---- 4. the weighted answers are still the answers ---- */
{
    /* all-ones == unweighted, bit-identically (the fix must not perturb) */
    const a = new LinearRegression().fit(X, y);
    const b = new LinearRegression().fit(X, y, { sampleWeight: [1, 1, 1, 1, 1, 1] });
    assert(a.coef.every((v, i) => Object.is(v, b.coef[i])) &&
           Object.is(a.intercept, b.intercept),
        "all-ones weights remain bit-identical to no weights");
}

print("test_ml_weights_churn: all " + n + " assertions passed");
