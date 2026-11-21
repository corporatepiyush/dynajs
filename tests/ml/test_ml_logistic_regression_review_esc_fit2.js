// covers: dyna:ml.LogisticRegression
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_logistic_regression_review_esc_fit2");
import { CSR, LinearRegression, LogisticRegression } from "dyna:ml";
let pass = 0, fail = 0;
const ok = (c, w, d) => { if (c) { pass++; print("  ok    " + w); }
                          else { fail++; print("  FAIL  " + w + (d ? "  [" + d + "]" : "")); } };

const X = [[1, 0, 0], [0, 1, 0], [0, 0, 1], [1, 1, 1], [2, 1, 0]];
const y = X.map(r => 3 * r[0] + 5 * r[1] + 7 * r[2]);
const want = [3, 5, 7];

function mkDup(X) {
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
function mkUnsorted(X) {
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
await probe.fit(X, y);

await __kitS.section("INFO logreg", async () => {
    const yl = y.map(v => (v > 7 ? 1 : 0));
    const gridq = [[1, 2, 1], [2, 0, 1], [0, 0, 1], [3, 3, 1]];
    const md = await new LogisticRegression({ maxIter: 300 }).fit(X, yl);
    const pd = await md.predictProba(gridq);
    let worst = 0;
    for (const [tag, S] of [["dup", mkDup(X)], ["unsorted", mkUnsorted(X)]]) {
        const m = await new LogisticRegression({ maxIter: 300 }).fit(S, yl);
        const ps = await m.predictProba(gridq);
        const d = Math.max(...ps.flatMap((r, i) => r.map((v, j) => Math.abs(v - pd[i][j]))));
        worst = Math.max(worst, d);
        print("INFO  logreg " + tag + "-expanded sparse fit: max proba diff " + d);
        S.close(); m.close();
    }
    ok(worst < 1e-6, "D4 logreg dup/unsorted sparse fit stays at dense-fit answers (max diff " + worst + ")");
    md.close();
});
__kitS.end();
print("review_esc_fit2: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("review_esc_fit2: " + fail + " failures");
