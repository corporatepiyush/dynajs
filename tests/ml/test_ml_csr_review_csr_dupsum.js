// covers: dyna:ml.CSR
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_csr_review_csr_dupsum");
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

await __kitS.section("P27 row() == toDense() with 2-5 duplicates (2-5 sum)", async () => {
P27: {
    const S = new CSR([2, 3, 1e16, 1, -1e16, 0.1, 0.2, 0.3, 5, 5, 5, 5, 5],
                      [0, 0,  1,   1,  1,    2,   2,   2,   3, 3, 3, 3, 3],
                      [0, 13], 4);
    const D = await S.toDense(), r = S.row(0);
    ok(same(r, D[0]) && Object.is(r[0], 5) && Object.is(r[3], 25),
       "P27 row() == toDense() with 2-5 duplicates (2-5 sum)",
       JSON.stringify(r) + " vs " + JSON.stringify(D[0]));
    const T = new CSR([1e16, 1], [0, 0], [0, 2], 1);
    const t = T.row(0)[0];
    ok(Object.is(t, 1e16 + 1) || Object.is(t, 1e16),
       "P27b duplicate sum follows storage order (" + t + ")", "got " + t);
    S.close(); T.close();
}
});

await __kitS.section("P36 LinearRegression CSR predict == dense predict bitwise under duplicate FP stress", async () => {
P36: {
    const m = new LinearRegression();
    await m.fit([[1, 0], [0, 1], [1, 1], [2, 1]], [3, 0, 3, 6]);
    const W = [1, 1e-16, 1e-17, 3, 7, 1e16, -1e16, 0.1, 1e8, 1e-8, 2, -1, 5];
    let diverge = null, checked = 0;
    outer:
    for (const a of W) for (const b of W) {
        const S = new CSR([a, b], [0, 0], [0, 2], 2);
        const D = await S.toDense();
        checked++;
        const sp = (await m.predict(S))[0], de = (await m.predict(D))[0];
        if (!Object.is(sp, de)) { diverge = { dups: [a, b], sparse: sp, dense: de }; break outer; }
        S.close();
    }
    if (!diverge) {
        outer2:
        for (const a of W) for (const b of W) for (const c of W) {
            const S = new CSR([a, b, c], [0, 0, 0], [0, 3], 2);
            const D = await S.toDense();
            checked++;
            const sp = (await m.predict(S))[0], de = (await m.predict(D))[0];
            if (!Object.is(sp, de)) {
                diverge = { dups: [a, b, c], sparse: sp, dense: de }; break outer2; }
            S.close();
        }
    }
    ok(diverge === null,
       "P36 LinearRegression CSR predict == dense predict bitwise under duplicate FP stress (" +
       checked + " duplicate rows checked)",
       diverge ? "dups " + JSON.stringify(diverge.dups) + ": sparse=" +
                 diverge.sparse + " dense=" + diverge.dense : "");
    if (diverge)
        print("  NOTE  P36 first divergence: new CSR(" + JSON.stringify(diverge.dups) +
              ",[0,0,...],[0," + diverge.dups.length + "],2)");
    m.close();
}
});
__kitS.end();

print("review_csr_dupsum: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("review_csr_dupsum: " + fail + " failures");
