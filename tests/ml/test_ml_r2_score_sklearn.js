// covers: dyna:ml.r2Score
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_r2_score_sklearn");
import * as ml from "dyna:ml";
import { DS, E } from "./sklearn_vectors.js";

let n = 0;
function assert(c, m) { n++; if (!c) throw new Error("assertion failed: " + m); }
function eq(got, want, m) {
    n++;
    if (got !== want) throw new Error("assertion failed: " + m +
        "\n  got:  " + got + "\n  want: " + want);
}
function near(got, want, tol, m) {
    n++;
    if (!(Math.abs(got - want) <= tol))
        throw new Error("assertion failed: " + m +
            "\n  got:  " + got + "\n  want: " + want + "\n  tol:   " + tol);
}
function maxdiff(a, b) {
    let d = 0;
    for (let i = 0; i < a.length; i++) d = Math.max(d, Math.abs(a[i] - b[i]));
    return d;
}
const F = (arr) => new Float64Array(arr);
const rowsOf = (flat, cols) => Array.from({ length: flat.length / cols },
    (_, i) => Array.from(flat.slice(i * cols, (i + 1) * cols)));
const cell = (m, i, k, cols) => Array.isArray(m[i]) ? m[i][k] : m[i * cols + k];

const T = (name, cols) => {
    const X = F(DS[name][0]);
    const y = DS[name][1] ? F(DS[name][1]) : null;
    return { X, y, rows: X.length / cols, cols };
};

await __kitS.section("r2 vs sklearn", async () => {
    const yt = [1, 1, 1, 0, 0, 0, 0, 0, 0, 0];
    const ys = [0.9, 0.8, 0.7, 0.6, 0.5, 0.4, 0.3, 0.2, 0.1, 0.05];
    const yp2 = [0.95, 0.85, 0.65, 0.55, 0.45, 0.35, 0.25, 0.15, 0.15, 0.05];
    const ypred = [1, 1, 0, 1, 0, 0, 0, 0, 0, 0];
    near(await ml.r2Score(yt, yp2), E.metrics.r2, 1e-12, "r2 vs sklearn");
    near(await ml.meanAbsoluteError(yt, ys), E.metrics.mae, 1e-12, "mae vs sklearn");
    near(await ml.meanSquaredError(yt, ys), E.metrics.mse, 1e-12, "mse vs sklearn");
    near(await ml.logLoss(yt, [0.9, 0.8, 0.7, 0.4, 0.3, 0.2, 0.1, 0.05, 0.15, 0.02]),
         E.metrics.logloss, 1e-12, "logLoss vs sklearn");
    near(await ml.accuracy(yt, ypred), E.metrics.acc, 1e-12, "accuracy vs sklearn");
    near(await ml.rocAuc(yt, ys), E.metrics.auc, 1e-12, "rocAuc vs sklearn");
    near(await ml.averagePrecision(yt, ys), E.metrics.ap, 1e-12,
         "averagePrecision vs sklearn");
    const cm = await ml.confusionMatrix(yt, ypred);
    const flat = [];
    for (const row of cm) for (const v of row) flat.push(v);
    assert(maxdiff(flat, E.metrics.conf) === 0, "confusionMatrix vs sklearn");
});
__kitS.end();

print("test_ml_sklearn: all " + n + " comparisons passed");
