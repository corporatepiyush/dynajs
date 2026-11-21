// covers: dyna:ml.LogisticRegression
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_logistic_regression_sklearn");
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

await __kitS.section("logreg proba vs sklearn", async () => {
    const { X, y, rows, cols } = T("clf", 3);
    const m = await new ml.LogisticRegression({ penalty: "l2", C: 1.0, tol: 1e-8,
                                          maxIter: 20000 })
        .fit(X, y, rows, cols);
    const proba = await m.predictProba(X, rows, cols);
    let d = 0;
    for (let i = 0; i < rows; i++)
        for (let k = 0; k < 2; k++)
            d = Math.max(d, Math.abs(cell(proba, i, k, 2) - E.log.proba[i * 2 + k]));
    assert(d <= 1e-6, "logreg proba vs sklearn (maxdiff " + d + ")");
    const pred = await m.predict(X, rows, cols);
    let fence = 0;
    for (let i = 0; i < rows; i++) {
        const p = E.log.proba[i * 2 + 1];
        if (Math.abs(p - 0.5) < 0.05) { fence++; continue; }
        eq(pred[i], E.log.pred[i], "logreg label vs sklearn @" + i);
    }
    assert(fence < rows / 5, "logreg fence rows: " + fence + " (data not separable)");
});
__kitS.end();

print("test_ml_sklearn: all " + n + " comparisons passed");
