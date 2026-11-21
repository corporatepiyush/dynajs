// covers: dyna:ml.LinearRegression
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_linear_regression_review_csr_dupsum");
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

await __kitS.section("P38 overflow-grade duplicates agree bitwise", async () => {
P38: {
    const m = new LinearRegression();
    await m.fit([[1, 0], [0, 1], [1, 1], [2, 1]], [3, 0, 3, 6]);
    const S = new CSR([1e300, 1e300, -1e300], [0, 0, 0], [0, 3], 2);
    const D = await S.toDense();
    const sp = (await m.predict(S))[0], de = (await m.predict(D))[0];
    ok(Object.is(sp, de),
       "P38 overflow-grade duplicates agree bitwise (sparse=" + sp + " dense=" + de + ")",
       "sparse and dense readings diverge");
    m.close(); S.close();
}
});
__kitS.end();

print("review_csr_dupsum: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("review_csr_dupsum: " + fail + " failures");
