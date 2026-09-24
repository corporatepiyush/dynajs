/* review_esc_fit2.js -- permanent row of the review lane's esc_fit2.js probe
 * (kept verbatim in behaviour; the forced-optimum rows it failed before the
 * fit consumed rows through their READING are green now).
 *
 * Original header: disambiguate the sparse-FIT divergence: exact, well-separated
 * systems where the dense fit recovers the FORCED optimum exactly. If the
 * sparse pair accumulation is merely a different FP order, its coefs match
 * within rounding of the 1e-9 ridge. If DUPLICATE or UNSORTED indices break it,
 * the coefs move grossly (wrong normal equations), conditioning-independent.
 */
import { CSR, LinearRegression, LogisticRegression } from "dyna:ml";
let pass = 0, fail = 0;
const ok = (c, w, d) => { if (c) { pass++; print("  ok    " + w); }
                          else { fail++; print("  FAIL  " + w + (d ? "  [" + d + "]" : "")); } };

/* y = 3*x0 + 5*x1 + 7*x2, intercept 0 -- exactly determined + 1 check row */
const X = [[1, 0, 0], [0, 1, 0], [0, 0, 1], [1, 1, 1], [2, 1, 0]];
const y = X.map(r => 3 * r[0] + 5 * r[1] + 7 * r[2]);
const want = [3, 5, 7];

function mkDup(X) {          /* 2-dup stacks, ascending columns */
    const val = [], col = [], ptr = [0];
    for (const r of X) {
        for (let c = 0; c < r.length; c++) {
            if (r[c] === 0) continue;
            val.push(r[c] / 2, r[c] / 2); col.push(c, c);
        }
        ptr.push(val.length);
    }
    return new CSR(val, col, ptr, X[0].length);
}
function mkUnsorted(X) {     /* descending columns within each row */
    const val = [], col = [], ptr = [0];
    for (const r of X) {
        const nz = [];
        for (let c = 0; c < r.length; c++) if (r[c] !== 0) nz.push([c, r[c]]);
        nz.reverse();
        for (const [c, v] of nz) { val.push(v); col.push(c); }
        ptr.push(val.length);
    }
    return new CSR(val, col, ptr, X[0].length);
}

const probe = new LinearRegression();
probe.fit(X, y);
ok(Math.abs(probe.coef[0] - 3) < 1e-6 && Math.abs(probe.coef[1] - 5) < 1e-6 &&
   Math.abs(probe.coef[2] - 7) < 1e-6,
   "D0 dense fit recovers the forced optimum (3, 5, 7): " + JSON.stringify(probe.coef));
const grid = [[1, 2], [2, 0], [0, 0], [3, 3]].map(r => [r[0], r[1], 1]);
const base = probe.predict([[1, 2, 1], [2, 0, 1], [0, 0, 1], [3, 3, 1]]);
probe.close();

/* 1. sorted-unique CSR (fromDense): FP-order divergence only */
{
    const S = CSR.fromDense(X);
    const m = new LinearRegression().fit(S, y);
    const cd = Math.max(...m.coef.map((v, i) => Math.abs(v - want[i])));
    print("INFO  sorted-unique sparse fit coef=" + JSON.stringify(m.coef) + " maxerr=" + cd);
    ok(cd < 1e-6, "D1 sorted-unique sparse fit recovers (3, 5, 7): maxerr " + cd);
    S.close(); m.close();
}
/* 2. DUPLICATE-expanded (same reading as X through row()/toDense()) */
{
    const S = mkDup(X);
    const rd = S.toDense();
    ok(rd[0][0] === X[0][0] && rd[3][1] === X[3][1], "D2 setup: dup reading == X bitwise");
    const m = new LinearRegression().fit(S, y);
    const cd = Math.max(...m.coef.map((v, i) => Math.abs(v - want[i])));
    print("INFO  duplicate-expanded sparse fit coef=" + JSON.stringify(m.coef) + " maxerr=" + cd);
    ok(cd < 1e-6, "D2 duplicate-expanded sparse fit recovers (3, 5, 7): maxerr " + cd,
       JSON.stringify(m.coef));
    S.close(); m.close();
}
/* 3. UNSORTED-within-row (same reading as X; test_ml_sparse pins predict
 * invariance to column order) */
{
    const S = mkUnsorted(X);
    const rd = S.toDense();
    ok(rd[0][0] === X[0][0] && rd[3][2] === X[3][2], "D3 setup: unsorted reading == X bitwise");
    const m = new LinearRegression().fit(S, y);
    const cd = Math.max(...m.coef.map((v, i) => Math.abs(v - want[i])));
    print("INFO  unsorted sparse fit coef=" + JSON.stringify(m.coef) + " maxerr=" + cd);
    ok(cd < 1e-6, "D3 unsorted sparse fit recovers (3, 5, 7): maxerr " + cd,
       JSON.stringify(m.coef));
    /* and its PREDICTIONS must match the dense fit's */
    const md = new LinearRegression().fit(X, y);
    const pd = md.predict([[1, 2, 1], [2, 0, 1], [0, 0, 1], [3, 3, 1]]);
    const ps = m.predict([[1, 2, 1], [2, 0, 1], [0, 0, 1], [3, 3, 1]]);
    ok(pd.every((v, i) => Math.abs(v - ps[i]) < 1e-6), "D3b unsorted-fit predictions == dense-fit",
       JSON.stringify(ps) + " vs " + JSON.stringify(pd));
    S.close(); m.close(); md.close();
}
/* 4. same shapes through LOGISTICRegression (the surviving dyn_csr_dot/
 * dyn_csr_axpy gradient walk) */
{
    const yl = y.map(v => (v > 7 ? 1 : 0));   /* both classes present */
    const gridq = [[1, 2, 1], [2, 0, 1], [0, 0, 1], [3, 3, 1]];
    const md = new LogisticRegression({ maxIter: 300 }).fit(X, yl);
    const pd = md.predictProba(gridq);
    let worst = 0;
    for (const [tag, S] of [["dup", mkDup(X)], ["unsorted", mkUnsorted(X)]]) {
        const m = new LogisticRegression({ maxIter: 300 }).fit(S, yl);
        const ps = m.predictProba(gridq);
        const d = Math.max(...ps.flatMap((r, i) => r.map((v, j) => Math.abs(v - pd[i][j]))));
        worst = Math.max(worst, d);
        print("INFO  logreg " + tag + "-expanded sparse fit: max proba diff " + d);
        S.close(); m.close();
    }
    ok(worst < 1e-6, "D4 logreg dup/unsorted sparse fit stays at dense-fit answers (max diff " + worst + ")");
    md.close();
}
print("review_esc_fit2: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("review_esc_fit2: " + fail + " failures");
