// covers: dyna:ml.GaussianMixture
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_gaussian_mixture_sklearn");
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

await __kitS.section("gmm log-likelihood vs sklearn", async () => {
    const { X, rows, cols } = T("gmm", 2);
    const m = await new ml.GaussianMixture(3, { regCovar: 1e-6, seed: 42 })
        .fit(X, rows, cols);
    near(m.logLikelihood, E.gmm.ll, 1e-6 * Math.abs(E.gmm.ll),
         "gmm log-likelihood vs sklearn");
});
__kitS.end();

print("test_ml_sklearn: all " + n + " comparisons passed");
