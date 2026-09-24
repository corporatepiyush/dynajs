/* probe_r3_ladder.js -- round-3 reproduction of the two confirmed regressions.
 *
 * REGRESSION 1: the conditioning gate at eps/DYN_RIDGE (~2.2e-7) ridges
 * WELL-POSED near-singular designs x2 = 3*x1 + d*z (z alternating +-1).
 * REGRESSION 2: the maxdiag-relative ridge kills mixed-unit scales (a constant
 * column at magnitude 1e4 / 1e6 collapses toward the zero model).
 *
 * Run: dynajs tests/agent/t2d_ml_df_time/probe_r3_ladder.js
 */
import { LinearRegression } from "dyna:ml";

function relerr(got, want) {
    return Math.abs(got - want) / Math.max(1, Math.abs(want));
}

/* ---- the near-singular ladder (reviewer's make_near, k=3 family) ---- */
const n = 8;
const x1 = Array.from({ length: n }, (_, i) => i + 1);
const z = [1, -1, 1, -1, 1, -1, 1, -1];
const dvals = [1, 2, 5, 10, 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000,
    20000, 50000, 100000, 200000, 500000, 1e6, 2e6, 5e6, 1e7,
    2e7, 5e7, 1e8, 5e8, 1e9, 1e10];

console.log("== REGRESSION 1: near-singular well-posed ladder (k=3) ==");
console.log("d          | null-target trainMaxRelErr | smooth trainMaxRelErr");
for (const d of dvals) {
    const x2 = x1.map((v, i) => 3 * v + d * z[i]);
    const X = x1.map((v, i) => [v, x2[i]]);
    const mNull = new LinearRegression().fit(X, z.map(zi => -d * zi));
    const tpN = mNull.predict(X);
    let wN = 0;
    for (let i = 0; i < n; i++)
        wN = Math.max(wN, relerr(tpN[i], -d * z[i]));
    const mS = new LinearRegression().fit(X, x1.map(v => 2 * v + 1));
    const tpS = mS.predict(X);
    let wS = 0;
    for (let i = 0; i < n; i++)
        wS = Math.max(wS, relerr(tpS[i], 2 * x1[i] + 1));
    console.log(d.toExponential(3).padEnd(11) + "| " + wN.toExponential(3).padEnd(27) + "| " + wS.toExponential(3));
}

/* ---- mixed-unit const columns ---- */
console.log("\n== REGRESSION 2: mixed-unit const-column designs (y = 2*x1 + 5) ==");
for (const M of [1e0, 1e2, 1e4, 1e6, 1e8]) {
    const X = [[1, M], [2, M], [3, M], [4, M]];
    const y = [7, 9, 11, 13]; /* = 2*x1 + 5 exactly */
    const m = new LinearRegression().fit(X, y);
    const tp = m.predict(X);
    let w = 0;
    for (let i = 0; i < 4; i++) w = Math.max(w, relerr(tp[i], y[i]));
    const pr = m.predict([[2.5, M]])[0];
    console.log("M=" + M.toExponential(0).padEnd(6) +
        " trainMaxRelErr=" + w.toExponential(3).padEnd(12) +
        " coef=[" + m.coef.map(v => v.toExponential(6)).join(", ") + "]" +
        " icpt=" + m.intercept.toExponential(6) +
        " probePred=" + pr.toExponential(6) + " (want 10)");
}

/* ---- the 1e300-weights x huge-features corner ---- */
console.log("\n== CORNER: 1e300 weights x 1e6-magnitude features ==");
try {
    const X = [[1, 1e6], [2, 1e6], [3, 1e6], [4, 1e6]];
    const y = [3, 5, 7, 9];
    const m = new LinearRegression().fit(X, y, { sampleWeight: [1e300, 1e300, 1e300, 1e300] });
    console.log("ok: coef=[" + m.coef.join(", ") + "] icpt=" + m.intercept +
        " train=" + JSON.stringify(m.predict(X)));
} catch (e) {
    console.log("THREW: " + String(e && e.message || e));
}
console.log("\n(unweighted 1e6 features, for reference)");
try {
    const X = [[1, 1e6], [2, 1e6], [3, 1e6], [4, 1e6]];
    const y = [3, 5, 7, 9];
    const m = new LinearRegression().fit(X, y);
    console.log("ok: coef=[" + m.coef.join(", ") + "] icpt=" + m.intercept +
        " train=" + JSON.stringify(m.predict(X)));
} catch (e) {
    console.log("THREW: " + String(e && e.message || e));
}
