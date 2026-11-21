// covers: dyna:ml.KNClassifier
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_kn_classifier_sklearn");
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

await __kitS.section("knn classifier labels vs sklearn", async () => {
    const { X, y, rows, cols } = T("knn", 3);
    const c = await new ml.KNClassifier(5).fit(X, y, rows, cols);
    assert(maxdiff(await c.predict(X, rows, cols), E.knn.clf) === 0,
           "knn classifier labels vs sklearn");
    const r = await new ml.KNRegressor(5).fit(X, y, rows, cols);
    assert(maxdiff(await r.predict(X, rows, cols), E.knn.reg) <= 1e-12,
           "knn regressor values vs sklearn");
});
__kitS.end();

print("test_ml_sklearn: all " + n + " comparisons passed");
