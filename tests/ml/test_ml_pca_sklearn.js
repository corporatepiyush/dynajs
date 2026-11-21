// covers: dyna:ml.PCA
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_pca_sklearn");
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

await __kitS.section("pca explained variance vs sklearn", async () => {
    const { X, rows, cols } = T("pca", 3);
    const m = await new ml.PCA(2).fit(X, rows, cols);
    assert(maxdiff(m.explainedVariance, E.pca.explained) <= 1e-9,
           "pca explained variance vs sklearn");
    const t = await m.transform(X, rows, cols);
    for (let c = 0; c < 2; c++) {
        let plus = 0, minus = 0;
        for (let i = 0; i < rows; i++) {
            const got = t[i * 2 + c], want = E.pca.transform[i * 2 + c];
            plus = Math.max(plus, Math.abs(got - want));
            minus = Math.max(minus, Math.abs(got + want));
        }
        assert(Math.min(plus, minus) <= 1e-9,
               "pca component " + c + " vs sklearn up to sign");
    }
});
__kitS.end();

print("test_ml_sklearn: all " + n + " comparisons passed");
