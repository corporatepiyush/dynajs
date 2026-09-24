/* review_csr_dupsum.js -- e7 review, the duplicate-index SUM rule (P27-P38).
 *
 * The contract: duplicate column indices within a row SUM (scipy's rule),
 * in row(), toDense() AND every sparse predict ALIKE. If any two walks of
 * the same matrix disagree, sparse and dense silently diverge into
 * different answers. row()/toDense()/dyn_ml_row_at accumulate `x[col] += v`
 * in storage order -- identical arithmetic. When this file was written,
 * LinearRegression and LogisticRegression predict over a CSR ran
 * dyn_csr_dot, which sums(v_k * w_c) per nonzero WITHOUT pre-summing
 * duplicates -- mathematically the same sum, NOT the same floating-point
 * computation -- and P36-P38 caught it. Those predicts now expand the row
 * through the shared scratch and run the dense kernel, and these probes pin
 * both sides bit-for-bit (Object.is), including adversarial values whose
 * sums overflow float precision when combined in different orders.
 */
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

/* training data: deterministic, dense */
const Xt = [[1, 0, 2], [0, 3, 0], [4, 0, 5], [1, 1, 1], [2, 2, 0], [0, 1, 3]];
const yt = [1, 2, 3, 2, 2, 3];
const yr = [1.5, 2.5, 3.5, 2.0, 2.2, 3.1];

P27: {
    /* row() and toDense() must combine duplicates IDENTICALLY */
    const S = new CSR([2, 3, 1e16, 1, -1e16, 0.1, 0.2, 0.3, 5, 5, 5, 5, 5],
                      [0, 0,  1,   1,  1,    2,   2,   2,   3, 3, 3, 3, 3],
                      [0, 13], 4);
    const D = S.toDense(), r = S.row(0);
    ok(same(r, D[0]) && Object.is(r[0], 5) && Object.is(r[3], 25),
       "P27 row() == toDense() with 2-5 duplicates (2-5 sum)",
       JSON.stringify(r) + " vs " + JSON.stringify(D[0]));
    /* the reading is SUM-IN-STORAGE-ORDER: pin it exactly */
    const T = new CSR([1e16, 1], [0, 0], [0, 2], 1);
    const t = T.row(0)[0];
    ok(Object.is(t, 1e16 + 1) || Object.is(t, 1e16),   /* (1e16+1) rounds to 1e16 */
       "P27b duplicate sum follows storage order (" + t + ")", "got " + t);
    S.close(); T.close();
}

/* per-estimator sparse==dense on DUPLICATE-laden queries */
function dupQuery() {
    /* one query row per estimator: duplicates at cols 0,1,2 plus ordering chaos */
    return new CSR(
        [1, 1e-16,  2,   2,   -3, 0.5,   7,  0.1, 0.2, 0.3,   4, 4, 4],
        [0, 0,      1,   1,    2, 2,     0,  1,   1,   1,     2, 2, 2],
        [0, 13], 3);
}
const dupDense = () => { const S = dupQuery(); const D = S.toDense(); S.close(); return D; };

P28: {
    const S = dupQuery();
    const m = new KMeans(3, 11).fit(Xt);
    ok(same(m.predict(S), m.predict(dupDense())),
       "P28 KMeans.predict: duplicates combine identically sparse vs dense");
    const into = new Float64Array(1);
    m.predictInto(into, S);
    ok(Object.is(into[0], m.predict(dupDense())[0]),
       "P28b KMeans.predictInto too");
    m.close(); S.close();
}
P29: {
    const S = dupQuery();
    const m = new DecisionTreeClassifier({ seed: 1 }).fit(Xt, yt);
    const m2 = new DecisionTreeRegressor({ seed: 1 }).fit(Xt, yr);
    ok(same(m.predict(S), m.predict(dupDense())) &&
       same(m2.predict(S), m2.predict(dupDense())),
       "P29 DecisionTree{C,R}.predict duplicate-consistent");
    m.close(); m2.close(); S.close();
}
P30: {
    const S = dupQuery();
    const m = new RandomForestClassifier({ nEstimators: 5, seed: 2 }).fit(Xt, yt);
    const m2 = new RandomForestRegressor({ nEstimators: 5, seed: 2 }).fit(Xt, yr);
    ok(same(m.predict(S), m.predict(dupDense())) &&
       same(m2.predict(S), m2.predict(dupDense())) &&
       same(m.predictProba(S), m.predictProba(dupDense())),
       "P30 RandomForest{C,R} predict+predictProba duplicate-consistent");
    m.close(); m2.close(); S.close();
}
P31: {
    const S = dupQuery();
    const m = new GradientBoostingClassifier({ nEstimators: 5, seed: 3 }).fit(Xt, yt);
    const m2 = new GradientBoostingRegressor({ nEstimators: 5, seed: 3 }).fit(Xt, yr);
    const m3 = new XGBClassifier({ nEstimators: 5, seed: 4 }).fit(Xt, yt);
    const m4 = new XGBRegressor({ nEstimators: 5, seed: 4 }).fit(Xt, yr);
    ok(same(m.predict(S), m.predict(dupDense())) &&
       same(m2.predict(S), m2.predict(dupDense())) &&
       same(m3.predict(S), m3.predict(dupDense())) &&
       same(m4.predict(S), m4.predict(dupDense())),
       "P31 GB{C,R} + XGB{C,R} duplicate-consistent");
    m.close(); m2.close(); m3.close(); m4.close(); S.close();
}
P32: {
    const S = dupQuery();
    const m = new GaussianNB().fit(Xt, yt);
    ok(same(m.predict(S), m.predict(dupDense())) &&
       same(m.predictProba(S), m.predictProba(dupDense())),
       "P32 GaussianNB predict+predictProba duplicate-consistent");
    m.close(); S.close();
}
P33: {
    const S = dupQuery();
    const m = new KNClassifier(3).fit(Xt, yt);
    const m2 = new KNRegressor(3, "distance").fit(Xt, yr);
    ok(same(m.predict(S), m.predict(dupDense())) &&
       same(m2.predict(S), m2.predict(dupDense())),
       "P33 KN{C,R}.predict duplicate-consistent");
    m.close(); m2.close(); S.close();
}
P34: {
    const S = dupQuery();
    const m = new SVC().fit(Xt, yt);
    ok(same(m.predict(S), m.predict(dupDense())) &&
       same(m.decisionFunction(S), m.decisionFunction(dupDense())),
       "P34 SVC predict+decisionFunction duplicate-consistent");
    m.close(); S.close();
}
P35: {
    const S = dupQuery();
    const m = new GaussianMixture(2, { seed: 6 }).fit(Xt);
    ok(same(m.predict(S), m.predict(dupDense())) &&
       same(m.predictProba(S), m.predictProba(dupDense())),
       "P35 GaussianMixture predict+predictProba duplicate-consistent");
    m.close(); S.close();
}

/* ---- the adversarial FP hunt: does ANY walk disagree? ------------------ */
P36: {
    /* LinearRegression/LogisticRegression predict over a CSR runs
     * dyn_csr_dot: sum_k(v_k * w) per duplicate -- while toDense() pre-sums
     * the v_k. Search duplicate 2- and 3-sets for a bitwise disagreement. */
    const m = new LinearRegression();
    m.fit([[1, 0], [0, 1], [1, 1], [2, 1]], [3, 0, 3, 6]);   /* coef ~ [3, 0] */
    const W = [1, 1e-16, 1e-17, 3, 7, 1e16, -1e16, 0.1, 1e8, 1e-8, 2, -1, 5];
    let diverge = null, checked = 0;
    outer:
    for (const a of W) for (const b of W) {
        const S = new CSR([a, b], [0, 0], [0, 2], 2);
        const D = S.toDense();
        checked++;
        const sp = m.predict(S)[0], de = m.predict(D)[0];
        if (!Object.is(sp, de)) { diverge = { dups: [a, b], sparse: sp, dense: de }; break outer; }
        S.close();
    }
    if (!diverge) {
        outer2:
        for (const a of W) for (const b of W) for (const c of W) {
            const S = new CSR([a, b, c], [0, 0, 0], [0, 3], 2);
            const D = S.toDense();
            checked++;
            const sp = m.predict(S)[0], de = m.predict(D)[0];
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
P37: {
    /* the same hunt through LogisticRegression predictProba + predict */
    const m = new LogisticRegression();
    m.fit([[1, 0], [0, 1], [1, 1], [2, 1], [0, 2]], [0, 1, 0, 0, 1]);
    const W = [1, 1e-16, 1e-17, 3, 7, 1e16, -1e16, 0.1, 1e8, 1e-8];
    let diverge = null, checked = 0;
    outer:
    for (const a of W) for (const b of W) {
        const S = new CSR([a, b], [0, 0], [0, 2], 2);
        const D = S.toDense();
        checked++;
        const sp = m.predictProba(S)[0], de = m.predictProba(D)[0];
        const sl = m.predict(S)[0], dl = m.predict(D)[0];
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
P38: {
    /* the OVERFLOW class: duplicates whose intermediate products overflow
     * to inf in one reading and not the other. dense = toDense() reading. */
    const m = new LinearRegression();
    m.fit([[1, 0], [0, 1], [1, 1], [2, 1]], [3, 0, 3, 6]);
    const S = new CSR([1e300, 1e300, -1e300], [0, 0, 0], [0, 3], 2);
    const D = S.toDense();
    const sp = m.predict(S)[0], de = m.predict(D)[0];
    ok(Object.is(sp, de),
       "P38 overflow-grade duplicates agree bitwise (sparse=" + sp + " dense=" + de + ")",
       "sparse and dense readings diverge");
    m.close(); S.close();
}

print("review_csr_dupsum: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("review_csr_dupsum: " + fail + " failures");
