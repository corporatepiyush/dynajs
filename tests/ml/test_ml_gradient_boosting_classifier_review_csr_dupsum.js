// covers: dyna:ml.GradientBoostingClassifier
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_gradient_boosting_classifier_review_csr_dupsum");
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

function dupQuery() {
    return new CSR(
        [1, 1e-16,  2,   2,   -3, 0.5,   7,  0.1, 0.2, 0.3,   4, 4, 4],
        [0, 0,      1,   1,    2, 2,     0,  1,   1,   1,     2, 2, 2],
        [0, 13], 3);
}

const dupDense = async () => { const S = dupQuery(); const D = await S.toDense(); S.close(); return D; };

await __kitS.section("P31 GB{C,R} + XGB{C,R} duplicate-consistent", async () => {
P31: {
    const S = dupQuery();
    const m = await new GradientBoostingClassifier({ nEstimators: 5, seed: 3 }).fit(Xt, yt);
    const m2 = await new GradientBoostingRegressor({ nEstimators: 5, seed: 3 }).fit(Xt, yr);
    const m3 = await new XGBClassifier({ nEstimators: 5, seed: 4 }).fit(Xt, yt);
    const m4 = await new XGBRegressor({ nEstimators: 5, seed: 4 }).fit(Xt, yr);
    ok(same(await m.predict(S), await m.predict(await dupDense())) &&
       same(await m2.predict(S), await m2.predict(await dupDense())) &&
       same(await m3.predict(S), await m3.predict(await dupDense())) &&
       same(await m4.predict(S), await m4.predict(await dupDense())),
       "P31 GB{C,R} + XGB{C,R} duplicate-consistent");
    m.close(); m2.close(); m3.close(); m4.close(); S.close();
}
});
__kitS.end();

print("review_csr_dupsum: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("review_csr_dupsum: " + fail + " failures");
