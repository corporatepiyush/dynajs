// covers: dyna:ml.SVC
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_svc_sklearn");
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

await __kitS.section("svc labels vs sklearn", async () => {
    const { X, y, rows, cols } = T("svc", 2);
    let v = 0;
    for (let i = 0; i < X.length; i++) v += X[i] * X[i];
    v /= X.length;
    const m = await new ml.SVC({ kernel: "rbf", C: 1.0, gamma: 1 / (2 * v) })
        .fit(X, y, rows, cols);
    assert(maxdiff(await m.predict(X, rows, cols), E.svc.pred) === 0,
           "svc labels vs sklearn");
});
__kitS.end();

print("test_ml_sklearn: all " + n + " comparisons passed");
