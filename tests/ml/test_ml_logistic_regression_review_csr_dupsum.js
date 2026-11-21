// covers: dyna:ml.LogisticRegression
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_logistic_regression_review_csr_dupsum");
import { CSR, LinearRegression, LogisticRegression, KMeans,
         DecisionTreeClassifier, DecisionTreeRegressor,
         RandomForestClassifier, RandomForestRegressor,
         GradientBoostingClassifier, GradientBoostingRegressor,
         XGBClassifier, XGBRegressor,
         GaussianNB, KNClassifier, KNRegressor, SVC, GaussianMixture } from "dyna:ml";

let pass = 0, fail = 0;
const ok = (c, w, d) => { if (c) { pass++; print("  ok    " + w); }
                          else { fail++; print("  FAIL  " + w + (d ? "  [" + d + "]" : "")); } };
const same = (a, b) => { const A = Array.from(a).flat(9), B = Array.from(b).flat(9);
    return A.length === B.length && A.every((v, i) => Object.is(v, B[i])); };

const Xt = [[1, 0, 2], [0, 3, 0], [4, 0, 5], [1, 1, 1], [2, 2, 0], [0, 1, 3]];
const yt = [1, 2, 3, 2, 2, 3];
const yr = [1.5, 2.5, 3.5, 2.0, 2.2, 3.1];

await __kitS.section("P37 LogisticRegression CSR predictProba/predict bitwise under duplicate FP stress", async () => {
P37: {
    const m = new LogisticRegression();
    await m.fit([[1, 0], [0, 1], [1, 1], [2, 1], [0, 2]], [0, 1, 0, 0, 1]);
    const W = [1, 1e-16, 1e-17, 3, 7, 1e16, -1e16, 0.1, 1e8, 1e-8];
    let diverge = null, checked = 0;
    outer:
    for (const a of W) for (const b of W) {
        const S = new CSR([a, b], [0, 0], [0, 2], 2);
        const D = await S.toDense();
        checked++;
        const sp = (await m.predictProba(S))[0], de = (await m.predictProba(D))[0];
        const sl = (await m.predict(S))[0], dl = (await m.predict(D))[0];
        if (!same(sp, de) || !Object.is(sl, dl)) {
            diverge = { dups: [a, b], sp, de, sl, dl }; break outer; }
        S.close();
    }
    ok(diverge === null,
       "P37 LogisticRegression CSR predictProba/predict bitwise under duplicate FP stress (" +
       checked + " rows)",
       diverge ? "dups " + JSON.stringify(diverge.dups) + " sparse=" +
                 JSON.stringify(diverge.sp) + " dense=" + JSON.stringify(diverge.de) : "");
    m.close();
}
});
__kitS.end();

print("review_csr_dupsum: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("review_csr_dupsum: " + fail + " failures");
