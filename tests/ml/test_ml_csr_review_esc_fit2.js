// covers: dyna:ml.CSR
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_csr_review_esc_fit2");
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

const grid = [[1, 2], [2, 0], [0, 0], [3, 3]].map(r => [r[0], r[1], 1]);

const base = await probe.predict([[1, 2, 1], [2, 0, 1], [0, 0, 1], [3, 3, 1]]);

probe.close();

await __kitS.section("INFO sorted-unique sparse fit coef", async () => {
    const S = await CSR.fromDense(X);
    const m = await new LinearRegression().fit(S, y);
    const cd = Math.max(...m.coef.map((v, i) => Math.abs(v - want[i])));
    print("INFO  sorted-unique sparse fit coef=" + JSON.stringify(m.coef) + " maxerr=" + cd);
    ok(cd < 1e-6, "D1 sorted-unique sparse fit recovers (3, 5, 7): maxerr " + cd);
    S.close(); m.close();
});
__kitS.end();
print("review_esc_fit2: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("review_esc_fit2: " + fail + " failures");
