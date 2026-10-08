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

function dupQuery() {
    return new CSR(
        [1, 1e-16,  2,   2,   -3, 0.5,   7,  0.1, 0.2, 0.3,   4, 4, 4],
        [0, 0,      1,   1,    2, 2,     0,  1,   1,   1,     2, 2, 2],
        [0, 13], 3);
}
const dupDense = async () => { const S = dupQuery(); const D = await S.toDense(); S.close(); return D; };

P28: {
    const S = dupQuery();
    const m = await new KMeans(3, 11).fit(Xt);
    ok(same(await m.predict(S), await m.predict(await dupDense())),
       "P28 KMeans.predict: duplicates combine identically sparse vs dense");
    const into = new Float64Array(1);
    await m.predictInto(into, S);
    ok(Object.is(into[0], (await m.predict(await dupDense()))[0]),
       "P28b KMeans.predictInto too");
    m.close(); S.close();
}
P29: {
    const S = dupQuery();
    const m = await new DecisionTreeClassifier({ seed: 1 }).fit(Xt, yt);
    const m2 = await new DecisionTreeRegressor({ seed: 1 }).fit(Xt, yr);
    ok(same(await m.predict(S), await m.predict(await dupDense())) &&
       same(await m2.predict(S), await m2.predict(await dupDense())),
       "P29 DecisionTree{C,R}.predict duplicate-consistent");
    m.close(); m2.close(); S.close();
}
P30: {
    const S = dupQuery();
    const m = await new RandomForestClassifier({ nEstimators: 5, seed: 2 }).fit(Xt, yt);
    const m2 = await new RandomForestRegressor({ nEstimators: 5, seed: 2 }).fit(Xt, yr);
    ok(same(await m.predict(S), await m.predict(await dupDense())) &&
       same(await m2.predict(S), await m2.predict(await dupDense())) &&
       same(await m.predictProba(S), await m.predictProba(await dupDense())),
       "P30 RandomForest{C,R} predict+predictProba duplicate-consistent");
    m.close(); m2.close(); S.close();
}
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
P32: {
    const S = dupQuery();
    const m = await new GaussianNB().fit(Xt, yt);
    ok(same(await m.predict(S), await m.predict(await dupDense())) &&
       same(await m.predictProba(S), await m.predictProba(await dupDense())),
       "P32 GaussianNB predict+predictProba duplicate-consistent");
    m.close(); S.close();
}
P33: {
    const S = dupQuery();
    const m = await new KNClassifier(3).fit(Xt, yt);
    const m2 = await new KNRegressor(3, "distance").fit(Xt, yr);
    ok(same(await m.predict(S), await m.predict(await dupDense())) &&
       same(await m2.predict(S), await m2.predict(await dupDense())),
       "P33 KN{C,R}.predict duplicate-consistent");
    m.close(); m2.close(); S.close();
}
P34: {
    const S = dupQuery();
    const m = await new SVC().fit(Xt, yt);
    ok(same(await m.predict(S), await m.predict(await dupDense())) &&
       same(await m.decisionFunction(S), await m.decisionFunction(await dupDense())),
       "P34 SVC predict+decisionFunction duplicate-consistent");
    m.close(); S.close();
}
P35: {
    const S = dupQuery();
    const m = await new GaussianMixture(2, { seed: 6 }).fit(Xt);
    ok(same(await m.predict(S), await m.predict(await dupDense())) &&
       same(await m.predictProba(S), await m.predictProba(await dupDense())),
       "P35 GaussianMixture predict+predictProba duplicate-consistent");
    m.close(); S.close();
}

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

print("review_csr_dupsum: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("review_csr_dupsum: " + fail + " failures");
