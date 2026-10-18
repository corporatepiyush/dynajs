import { CSR, LinearRegression, LogisticRegression, KMeans, PCA, GaussianNB,
         DecisionTreeClassifier, DecisionTreeRegressor, RandomForestClassifier,
         RandomForestRegressor, GradientBoostingClassifier, GradientBoostingRegressor,
         XGBClassifier, XGBRegressor, KNClassifier, KNRegressor, SVC,
         GaussianMixture, StandardScaler, accuracy } from "dyna:ml";

let n = 0;
function assert(c, m) { n++; if (!c) throw new Error("assertion failed: " + m); }
function close(a, b, eps, m) {
    n++;
    if (!(Math.abs(a - b) <= eps))
        throw new Error((m || "not close") + ": " + a + " vs " + b);
}
async function throws(fn, kind, m) {
    n++;
    try { await fn(); } catch (e) {
        if (kind && !(e instanceof kind)) throw new Error((m || "wrong error") + ": " + e);
        return String(e.message || e);
    }
    throw new Error((m || "expected a throw") + " but none happened");
}
function lcg(seed) { let s = seed >>> 0; return () => (s = (s * 1664525 + 1013904223) >>> 0) / 4294967296; }

function sparseData(seed, rows, cols, nzPerRow) {
    const rnd = lcg(seed), dense = [], y = [], yc = [];
    for (let i = 0; i < rows; i++) {
        const r = new Array(cols).fill(0);
        for (let k = 0; k < nzPerRow; k++)
            r[Math.floor(rnd() * cols)] = rnd() * 2 - 1;
        dense.push(r);
        const s = 3 * r[0] - 2 * r[1] + r[2];
        y.push(s + (rnd() * 2 - 1) * 0.05);
        yc.push(s > 0 ? 1 : 0);
    }
    return { dense, y, yc };
}

{
    const m = new CSR([1, 2, 3], [0, 2, 1], [0, 2, 3], 4);
    assert(m.rows === 2, "rows come from the pointer array");
    assert(m.cols === 4, "cols are given");
    assert(m.nnz === 3, "nnz is the value count");
    close(m.density, 3 / 8, 0, "density");
    assert(JSON.stringify(m.row(0)) === "[1,0,2,0]", "row 0");
    assert(JSON.stringify(m.row(1)) === "[0,3,0,0]", "row 1");
    assert(JSON.stringify(await m.toDense()) === "[[1,0,2,0],[0,3,0,0]]", "toDense");

    const e = new CSR([5], [1], [0, 0, 1], 3);
    assert(JSON.stringify(e.row(0)) === "[0,0,0]", "an empty row is all zeros");
    assert(JSON.stringify(e.row(1)) === "[0,5,0]", "and the next row is not");

    const z = new CSR([], [], [0, 0, 0], 2);
    assert(z.nnz === 0 && z.rows === 2, "an all-zero matrix is representable");
    close(z.density, 0, 0, "and has zero density");
    m.close(); e.close(); z.close();
}

{
    const d = sparseData(9, 200, 40, 3);
    const S = await CSR.fromDense(d.dense);
    assert(S.rows === 200 && S.cols === 40, "shape survives");
    assert(S.nnz <= 200 * 3, "no more nonzeros than were written");
    const back = await S.toDense();
    let exact = true;
    for (let i = 0; i < 200; i++)
        for (let j = 0; j < 40; j++)
            if (back[i][j] !== d.dense[i][j]) exact = false;
    assert(exact, "toDense(fromDense(X)) is X, bit for bit");
    for (let i = 0; i < 5; i++)
        assert(S.row(i).every((v, j) => v === d.dense[i][j]), "row " + i);

    const odd = await CSR.fromDense([[0, -0, 1e-300]]);
    assert(odd.nnz === 1, "-0 is a zero: " + odd.nnz);
    S.close(); odd.close();
}

{
    for (const [rows, cols, nz] of [[400, 60, 4], [150, 20, 2], [60, 200, 3]]) {
        const d = sparseData(11 + cols, rows, cols, nz);
        const S = await CSR.fromDense(d.dense);
        const tag = rows + "x" + cols;

        const a = await new LinearRegression().fit(d.dense, d.y);
        const b = await new LinearRegression().fit(S, d.y);
        const pa = await a.predict(d.dense), pb = await b.predict(d.dense);
        let worst = 0;
        for (let i = 0; i < pa.length; i++) worst = Math.max(worst, Math.abs(pa[i] - pb[i]));
        close(worst, 0, 1e-9, tag + ": LinearRegression sparse equals dense");

        const c = await new LogisticRegression({ maxIter: 200 }).fit(d.dense, d.yc);
        const e = await new LogisticRegression({ maxIter: 200 }).fit(S, d.yc);
        const qa = (await c.predictProba(d.dense)).map(r => r[1]);
        const qb = (await e.predictProba(d.dense)).map(r => r[1]);
        let w2 = 0;
        for (let i = 0; i < qa.length; i++) w2 = Math.max(w2, Math.abs(qa[i] - qb[i]));
        close(w2, 0, 1e-9, tag + ": LogisticRegression sparse equals dense");
        a.close(); b.close(); c.close(); e.close(); S.close();
    }
}

{
    const d = sparseData(21, 300, 30, 3);
    const S = await CSR.fromDense(d.dense);
    const w = d.y.map((_, i) => (i % 2 ? 2 : 0.5));

    const a = await new LinearRegression().fit(d.dense, d.y, { sampleWeight: w });
    const b = await new LinearRegression().fit(S, d.y, { sampleWeight: w });
    const pa = await a.predict(d.dense), pb = await b.predict(d.dense);
    let worst = 0;
    for (let i = 0; i < pa.length; i++) worst = Math.max(worst, Math.abs(pa[i] - pb[i]));
    close(worst, 0, 1e-9, "sampleWeight works on the sparse path too");

    const y3 = d.dense.map(r => (r[0] > 0.3 ? 2 : (r[1] > 0.3 ? 1 : 0)));
    const c = await new LogisticRegression({ maxIter: 200 }).fit(d.dense, y3);
    const e = await new LogisticRegression({ maxIter: 200 }).fit(S, y3);
    close(await accuracy(y3, await c.predict(d.dense)), await accuracy(y3, await e.predict(d.dense)), 1e-12,
          "multiclass sparse equals dense");

    assert((await b.predict([d.dense[0]])).length === 1, "a sparsely fitted model predicts densely");
    a.close(); b.close(); c.close(); e.close(); S.close();
}

{
    const S = await CSR.fromDense([[1, 0], [0, 2], [3, 0], [0, 4]]);
    const y = [1, 2, 3, 4], yc = [0, 1, 0, 1];

    const msg = await throws(async () => await new KMeans(2).fit(S), TypeError, "KMeans refuses a CSR");
    assert(msg.includes("toDense"), "and names toDense(): " + msg);

    await throws(async () => await new PCA(1).fit(S), TypeError, "PCA refuses");
    await throws(async () => await new GaussianNB().fit(S, yc), TypeError, "GaussianNB refuses");
    await throws(async () => await new DecisionTreeClassifier().fit(S, yc), TypeError, "the tree refuses");
    await throws(async () => await new RandomForestRegressor({ nEstimators: 2 }).fit(S, y), TypeError,
           "the forest refuses");
    await throws(async () => await new XGBRegressor({ nEstimators: 2 }).fit(S, y), TypeError, "XGB refuses");
    await throws(async () => await new StandardScaler().fit(S), TypeError, "the scaler refuses");

    assert((await (await new KMeans(2).fit(await S.toDense())).predict(await S.toDense())).length === 4,
           "toDense() makes it work");
    S.close();
}

{
    await throws(() => new CSR([1], [9], [0, 1], 4), RangeError, "a column outside the matrix");
    await throws(() => new CSR([1], [-1], [0, 1], 4), RangeError, "a negative column");
    await throws(() => new CSR([1], [0.5], [0, 1], 4), RangeError, "a fractional column");
    await throws(() => new CSR([1], [0], [1, 1], 4), RangeError, "pointers not starting at 0");
    await throws(() => new CSR([1], [0], [0, 2], 4), RangeError, "pointers past the value count");
    await throws(() => new CSR([1, 2], [0, 1], [0, 2, 1], 4), RangeError, "decreasing pointers");
    await throws(() => new CSR([1], [0, 1], [0, 1], 4), TypeError, "columns longer than values");
    await throws(() => new CSR([NaN], [0], [0, 1], 4), RangeError, "a NaN value");
    await throws(() => new CSR([1], [0], [0, 1], 0), RangeError, "zero columns");
    await throws(() => new CSR([1], [0], [], 4), RangeError, "no row pointers");
    await throws(() => new CSR([1], [0]), TypeError, "too few arguments");

    const m = await throws(() => new CSR([1, 2, 3], [0, 7, 1], [0, 3], 4), RangeError, "names it");
    assert(m.includes("[1]"), "the message names the index: " + m);
}

{
    const S = await CSR.fromDense([[1, 0], [0, 2]]);
    await throws(() => S.row(2), RangeError, "row past the end");
    await throws(() => S.row(-1), RangeError, "a negative row");

    let ran = 0;
    const bomb = { valueOf() { ran++; S.close(); return 0; } };
    await throws(() => S.row(bomb), Error, "closing during the index coercion");
    assert(ran > 0, "the attack ran: " + ran);

    const T = await CSR.fromDense([[1, 0], [0, 2], [3, 0]]);
    await throws(async () => await new LinearRegression().fit(T, [1, 2]), TypeError, "y too short");
    T.close();
}

{
    const S = await CSR.fromDense([[1, 0], [0, 2]]);
    S.close();
    S.close();
    n++;
    await throws(() => S.row(0), Error, "using a closed CSR throws");
    await throws(async () => await new LinearRegression().fit(S, [1, 2]), Error, "and so does fitting one");
}

{
    const d = sparseData(31, 120, 12, 3);
    const S = await CSR.fromDense(d.dense);
    const Q = await CSR.fromDense(d.dense.slice(0, 9));
    const eqOut = (a, b, tag) => {
        const A = Array.from(a), B = Array.from(b);
        assert(A.length === B.length &&
               A.every((v, i) => Object.is(v, B[i])),
               tag + ": " + JSON.stringify(A) + " vs " + JSON.stringify(B));
    };

    const km = await new KMeans(3, 5).fit(d.dense);
    eqOut(await km.predict(S), await km.predict(d.dense), "KMeans.predict");
    const into = new Float64Array(9);
    assert((await km.predictInto(into, Q)) === 9, "KMeans.predictInto(CSR) writes all rows");
    eqOut(into, (await km.predict(d.dense)).slice(0, 9), "KMeans.predictInto values");

    const dtc = await new DecisionTreeClassifier({ seed: 1 }).fit(d.dense, d.yc);
    eqOut(await dtc.predict(S), await dtc.predict(d.dense), "DecisionTreeClassifier.predict");
    const dtr = await new DecisionTreeRegressor({ seed: 1 }).fit(d.dense, d.y);
    eqOut(await dtr.predict(S), await dtr.predict(d.dense), "DecisionTreeRegressor.predict");

    const rfc = await new RandomForestClassifier({ nEstimators: 5, seed: 2 }).fit(d.dense, d.yc);
    eqOut(await rfc.predict(S), await rfc.predict(d.dense), "RandomForestClassifier.predict");
    const rfr = await new RandomForestRegressor({ nEstimators: 5, seed: 2 }).fit(d.dense, d.y);
    eqOut(await rfr.predict(S), await rfr.predict(d.dense), "RandomForestRegressor.predict");
    const pbS = await rfc.predictProba(S), pbD = await rfc.predictProba(d.dense);
    assert(Array.isArray(pbS[0]), "a CSR predictProba answers in the row-arrays form");
    assert(pbS.every((r, i) => r.every((v, j) => Object.is(v, pbD[i][j]))),
           "RandomForestClassifier.predictProba: CSR == dense, row for row");

    const gbr = await new GradientBoostingRegressor({ nEstimators: 5, seed: 3 }).fit(d.dense, d.y);
    eqOut(await gbr.predict(S), await gbr.predict(d.dense), "GradientBoostingRegressor.predict");
    const gbc = await new GradientBoostingClassifier({ nEstimators: 5, seed: 3 }).fit(d.dense, d.yc);
    eqOut(await gbc.predict(S), await gbc.predict(d.dense), "GradientBoostingClassifier.predict");
    const xgbr = await new XGBRegressor({ nEstimators: 5, seed: 4 }).fit(d.dense, d.y);
    eqOut(await xgbr.predict(S), await xgbr.predict(d.dense), "XGBRegressor.predict");
    const xgbc = await new XGBClassifier({ nEstimators: 5, seed: 4 }).fit(d.dense, d.yc);
    eqOut(await xgbc.predict(S), await xgbc.predict(d.dense), "XGBClassifier.predict");

    const nb = await new GaussianNB().fit(d.dense, d.yc);
    eqOut(await nb.predict(S), await nb.predict(d.dense), "GaussianNB.predict");
    const nbpS = await nb.predictProba(S), nbpD = await nb.predictProba(d.dense);
    assert(nbpS.every((r, i) => r.every((v, j) => Object.is(v, nbpD[i][j]))),
           "GaussianNB.predictProba: CSR == dense");

    const kc = await new KNClassifier(3).fit(d.dense, d.yc);
    eqOut(await kc.predict(S), await kc.predict(d.dense), "KNClassifier.predict");
    const kr = await new KNRegressor(3, "distance").fit(d.dense, d.y);
    eqOut(await kr.predict(Q), await kr.predict(d.dense.slice(0, 9)), "KNRegressor.predict");

    const svc = await new SVC().fit(d.dense, d.yc);
    eqOut(await svc.predict(S), await svc.predict(d.dense), "SVC.predict");
    const dfS = await svc.decisionFunction(S), dfD = await svc.decisionFunction(d.dense);
    eqOut(Array.isArray(dfS) ? dfS.flat() : dfS, Array.isArray(dfD) ? dfD.flat() : dfD,
          "SVC.decisionFunction");

    const gmm = await new GaussianMixture(2, { seed: 6 }).fit(d.dense);
    eqOut(await gmm.predict(S), await gmm.predict(d.dense), "GaussianMixture.predict");
    const gpS = await gmm.predictProba(S), gpD = await gmm.predictProba(d.dense);
    assert(gpS.every((r, i) => r.every((v, j) => Object.is(v, gpD[i][j]))),
           "GaussianMixture.predictProba: CSR == dense");

    km.close(); dtc.close(); dtr.close(); rfc.close(); rfr.close();
    gbr.close(); gbc.close(); xgbr.close(); xgbc.close(); nb.close();
    kc.close(); kr.close(); svc.close(); gmm.close();
    S.close(); Q.close();
}

{
    const d = sparseData(41, 40, 6, 2);
    const S = await CSR.fromDense(d.dense);
    const kc = await new KNClassifier(3).fit(d.dense, d.yc);

    await throws(() => new CSR([1, NaN], [0, 1], [0, 2], 6), RangeError,
           "a NaN value is refused at construction");
    await throws(async () => await CSR.fromDense([[1, NaN, 0, 0, 0, 0]]), RangeError,
           "a NaN value is refused by fromDense too");
    const N = new CSR([1e308, 1e308], [0, 0], [0, 2], 6);
    const msg = await throws(async () => await kc.predict(N), RangeError, "an overflow-class row is refused");
    assert(/NaN or infinite/.test(msg), "the refusal names the values: " + msg);

    const W = await CSR.fromDense([[1, 0]]);
    await throws(async () => await kc.predict(W), TypeError, "a CSR with the wrong width");

    const out = new Float64Array(2);
    await throws(async () => await kc.predictInto(out, S), RangeError, "a short out buffer");
    assert(out[0] === 0, "and nothing was written");

    await throws(async () => await new KNClassifier(3).predict(S), Error, "predict before fit");
    const C = await CSR.fromDense([[1, 0, 0, 0, 0, 0]]);
    C.close();
    await throws(async () => await kc.predict(C), Error, "a closed CSR");

    const E = new CSR([], [], [0], 6);
    assert((await kc.predict(E)).length === 0, "an empty CSR predicts no rows");
    const Z = new CSR([], [], [0, 0], 6);
    assert((await kc.predict(Z))[0] === (await kc.predict([[0, 0, 0, 0, 0, 0]]))[0],
           "an empty row is an all-zero row");

    const dup = new CSR([2, 3], [0, 0], [0, 2], 6);
    assert((await dup.toDense())[0][0] === 5 && dup.row(0)[0] === 5,
           "toDense/row sum duplicate indices");
    assert((await kc.predict(dup))[0] === (await kc.predict(await dup.toDense()))[0],
           "predict reads duplicates the same way toDense does");
    const uns = new CSR([1, 1], [5, 0], [0, 2], 6);
    assert((await kc.predict(uns))[0] === (await kc.predict(await uns.toDense()))[0],
           "column order within a row is irrelevant");

    kc.close(); S.close(); N.close(); W.close(); E.close(); Z.close();
    dup.close(); uns.close();
}

{
    const flat = (a) => Array.from(a).flat(9);
    const same = (a, b) => {
        const A = flat(a), B = flat(b);
        return A.length === B.length && A.every((v, i) => Object.is(v, B[i]));
    };
    const dupSplit = (v, k) => {
        const out = [v / Math.pow(2, k - 1), v / Math.pow(2, k - 1)];
        for (let j = k - 2; j >= 1; j--) out.push(v / Math.pow(2, j));
        return out;
    };
    const dupRow = (reading, k) => {
        const val = [], col = [], ptr = [0];
        for (let c = 0; c < reading.length; c++) {
            const parts = dupSplit(reading[c], ((c % (k - 1)) + 2));
            for (const p of parts) { val.push(p); col.push(c); }
        }
        ptr.push(val.length);
        return new CSR(val, col, ptr, reading.length);
    };

    {
        const S = new CSR([2, 3, 5, 5, 5, 5, 5, 0.5, 0.25, 0.25, 7, -7, 1, 1, 1],
                          [0, 0, 1, 1, 1, 1, 1, 2,    2,    2,    3, 3,  4, 4, 4],
                          [0, 15], 5);
        const r = S.row(0), D = (await S.toDense())[0];
        assert(same(r, D), "11a row() == toDense() bitwise with 2-5 duplicates");
        assert(Object.is(r[0], 5) && Object.is(r[1], 25) && Object.is(r[2], 1) &&
               Object.is(r[3], 0) && Object.is(r[4], 3),
               "11a 2-5 duplicate sums are exact (5, 25, 1, 0, 3): " + JSON.stringify(r));
        const T = new CSR([1e16, 1], [0, 0], [0, 2], 1);
        const t = T.row(0)[0];
        assert(Object.is(t, 1e16 + 1) || Object.is(t, 1e16),
               "11a duplicate sum follows storage order: " + t);
        for (const v of [3, 0.1, -7.5, 1e10, 1e-10])
            for (let k = 2; k <= 5; k++) {
                const parts = dupSplit(v, k);
                const D2 = new CSR(parts, parts.map(() => 0), [0, k], 1);
                assert(Object.is(D2.row(0)[0], v),
                       "11a dupSplit(" + v + ", " + k + ") sums to v bitwise");
                D2.close();
            }
        S.close(); T.close();
    }

    {
        const Xt = [[1, 0, 2], [0, 3, 0], [4, 0, 5], [1, 1, 1], [2, 2, 0], [0, 1, 3]];
        const yt = [1, 2, 3, 2, 2, 3], yr = [1.5, 2.5, 3.5, 2.0, 2.2, 3.1];
        const Q = dupRow([2.3, -1.25, 0.75], 5);
        const QD = await Q.toDense();
        const eqVals = async (name, fn) =>
            assert(same(await fn(Q), await fn(QD)), "11b " + name + ": dup-laden CSR == dense, bit for bit");
        const lin = await new LinearRegression().fit(Xt, yr);
        const log = await new LogisticRegression({ maxIter: 400 }).fit(Xt, yt);
        const kr = await new KNRegressor(3).fit(Xt, yr);
        const nb = await new GaussianNB().fit(Xt, yt);
        const svc = await new SVC().fit(Xt, yt);
        const gmm = await new GaussianMixture(2, { seed: 6 }).fit(Xt);
        const km = await new KMeans(3, 7).fit(Xt);
        const dtr = await new DecisionTreeRegressor({ seed: 1 }).fit(Xt, yr);
        const rfr = await new RandomForestRegressor({ nEstimators: 4, seed: 2 }).fit(Xt, yr);
        const gbr = await new GradientBoostingRegressor({ nEstimators: 4, seed: 3 }).fit(Xt, yr);
        await eqVals("LinearRegression.predict", async (X) => await lin.predict(X));
        await eqVals("LogisticRegression.predict", async (X) => await log.predict(X));
        await eqVals("LogisticRegression.predictProba", async (X) => await log.predictProba(X));
        await eqVals("KNRegressor.predict", async (X) => await kr.predict(X));
        await eqVals("GaussianNB.predictProba", async (X) => await nb.predictProba(X));
        await eqVals("SVC.decisionFunction", async (X) => await svc.decisionFunction(X));
        await eqVals("GaussianMixture.predictProba", async (X) => await gmm.predictProba(X));
        await eqVals("KMeans.predict", async (X) => await km.predict(X));
        await eqVals("DecisionTreeRegressor.predict", async (X) => await dtr.predict(X));
        await eqVals("RandomForestRegressor.predict", async (X) => await rfr.predict(X));
        await eqVals("GradientBoostingRegressor.predict", async (X) => await gbr.predict(X));
        {
            const a = new Float64Array(1), b = new Float64Array(1);
            await lin.predictInto(a, Q);
            await lin.predictInto(b, QD);
            assert(Object.is(a[0], b[0]), "11b LinearRegression.predictInto too");
        }
        lin.close(); log.close(); kr.close(); nb.close(); svc.close();
        gmm.close(); km.close(); dtr.close(); rfr.close(); gbr.close();
        Q.close();
    }

    {
        const lin = new LinearRegression();
        await lin.fit([[1, 0], [0, 1], [1, 1], [2, 1]], [3, 0, 3, 6]);
        const log = new LogisticRegression();
        await log.fit([[1, 0], [0, 1], [1, 1], [2, 1], [0, 2]], [0, 1, 0, 0, 1]);
        const W = [1, 1e-16, 1e-17, 3, 7, 1e16, -1e16, 0.1, 1e8, 1e-8, 2, -1, 5];
        let diverge = null, checked = 0;
        const hunt = async (fn) => {
            for (const a of W) for (const b of W) {
                const S = new CSR([a, b], [0, 0], [0, 2], 2);
                const D = await S.toDense();
                checked++;
                const sp = flat(await fn(S)), de = flat(await fn(D));
                if (!same(sp, de)) {
                    diverge = { dups: [a, b], sp, de };
                    S.close();
                    return;
                }
                S.close();
            }
            for (const a of W) for (const b of W) for (const c of [1, 1e-16, 3, 1e16, 0.1]) {
                const S = new CSR([a, b, c], [0, 0, 0], [0, 3], 2);
                const D = await S.toDense();
                checked++;
                const sp = flat(await fn(S)), de = flat(await fn(D));
                if (!same(sp, de)) {
                    diverge = { dups: [a, b, c], sp, de };
                    S.close();
                    return;
                }
                S.close();
            }
        };
        await hunt(async (X) => await lin.predict(X));
        if (!diverge) await hunt(async (X) => await log.predictProba(X));
        if (!diverge) await hunt(async (X) => await log.predict(X));
        assert(diverge === null,
               "11c lin/logreg CSR predict bitwise-identical to dense under duplicate FP stress (" +
               checked + " rows)",
               diverge ? "dups " + JSON.stringify(diverge.dups) + " sparse=" +
                         JSON.stringify(diverge.sp) + " dense=" + JSON.stringify(diverge.de) : "");
        {
            const S = new CSR([1e300, 1e300, -1e300], [0, 0, 0], [0, 3], 2);
            const D = await S.toDense();
            assert(same(await lin.predict(S), await lin.predict(D)),
                   "11c overflow-grade duplicates agree bitwise (predict=" +
                   (await lin.predict(S)) + ")");
            S.close();
        }
        lin.close(); log.close();
    }

    {
        const H = new CSR([1.0], [0], [0, 1], Math.pow(2, 40));
        const e1 = await throws(() => H.row(0), RangeError, "11d huge-width row() throws");
        assert(/fit in memory/.test(e1), "11d row() names the memory bound: " + e1);
        const e2 = await throws(async () => await H.toDense(), RangeError, "11d huge-width toDense() throws");
        assert(/fit in memory/.test(e2), "11d toDense() names the memory bound: " + e2);
        const lin = await new LinearRegression().fit([[1, 0, 0], [0, 1, 0], [0, 0, 1]], [1, 2, 3]);
        const e3 = await throws(async () => await lin.predict(H), TypeError, "11d huge-width predict refuses");
        assert(e3.includes("1099511627776"),
               "11d the width message prints the full 64-bit width: " + e3);
        const fitWide = await (async () => {
            const S = new CSR([1.0], [0], [0, 1], Math.pow(2, 40));
            const mm = new LinearRegression();
            let r;
            try { await mm.fit(S, [1]); r = "ACCEPTED"; } catch (e) { r = e.constructor.name; }
            S.close(); mm.close();
            return r;
        })();
        assert(fitWide === "RangeError",
               "11d sparse fit on 2^40 cols refuses with RangeError: " + fitWide);
        H.close(); lin.close();
    }
}

{
    const flat = (a) => Array.from(a).flat(9);
    const dupSplit = (v, k) => {
        const out = [v / Math.pow(2, k - 1), v / Math.pow(2, k - 1)];
        for (let j = k - 2; j >= 1; j--) out.push(v / Math.pow(2, j));
        return out;
    };
    const dupQuery = (reading, k) => {
        const val = [], col = [], ptr = [0];
        for (let c = 0; c < reading.length; c++)
            for (const p of dupSplit(reading[c], k)) { val.push(p); col.push(c); }
        ptr.push(val.length);
        return new CSR(val, col, ptr, reading.length);
    };
    const nearArr = (got, want, tol, m) => {
        const G = flat(got), W = flat(want);
        let maxdiff = 0;
        for (let i = 0; i < W.length; i++)
            maxdiff = Math.max(maxdiff, Math.abs((G[i] || 0) - W[i]));
        assert(G.length === W.length && maxdiff <= tol,
               m + " (got " + JSON.stringify(G) + ", want " + JSON.stringify(W) +
               ", maxdiff " + maxdiff + ")");
    };
    const sameSort = (got, want, tol, m) => {
        const G = flat(got).slice().sort((a, b) => a - b);
        const W = flat(want).slice().sort((a, b) => a - b);
        nearArr(G, W, tol, m);
    };

    {
        const m = new LinearRegression();
        await m.fit([[1, 0], [0, 1], [1, 1], [2, 1]], [10, 12, 15, 18]);
        nearArr(m.coef, [3, 5], 1e-6, "12 lin coefs = (3, 5)");
        close(m.intercept, 7, 1e-6, "12 lin intercept = 7");
        nearArr(await m.predict([[1, 2], [2, 0]]), [20, 13], 1e-6,
               "12 lin predict = 3x0+5x1+7 (numpy arithmetic)");
        const Q = dupQuery([1, 2], 4);
        nearArr(await m.predict(Q), [20], 1e-6, "12 lin predict on a 4-duplicate query = 20");
        m.close(); Q.close();
    }

    {
        const m = new LogisticRegression();
        await m.fit([[-2], [-1], [1], [2]], [0, 1, 0, 1]);
        nearArr(await m.predictProba([[0]]), [[0.5, 0.5]], 1e-9,
               "12 logreg symmetric proba at 0 = [0.5, 0.5]");
        for (const x of [0.5, 1, 2]) {
            const p = await m.predictProba([[x], [-x]]);
            close(p[0][1] + p[1][1], 1, 1e-9, "12 logreg p(" + x + ")+p(-" + x + ")=1");
        }
        const Q = new CSR([1.5, -1.5], [0, 0], [0, 2], 1);
        nearArr(await m.predictProba(Q), [[0.5, 0.5]], 1e-9,
               "12 logreg proba on a duplicate row reading 0 = [0.5, 0.5]");
        assert((await m.predict([[-2]]))[0] === 0 && (await m.predict([[2]]))[0] === 1,
               "12 logreg labels follow the (w > 0) optimum");
        m.close(); Q.close();
    }

    {
        const X = [[-3], [-1], [0], [2.5], [5]];
        const kr = await new KNRegressor(3).fit(X, [1, 2, 3, 10, 20]);
        const kc = await new KNClassifier(3).fit(X, [0, 0, 1, 1, 1]);
        nearArr(await kr.predict([[3.5]]), [11], 0, "12 knn-reg k=3 uniform = 11 exactly");
        assert((await kc.predict([[-0.4]]))[0] === 0, "12 knn-clf majority = 0");
        const Q = dupQuery([3.5], 5), Q2 = dupQuery([-0.4], 5);
        nearArr(await kr.predict(Q), [11], 0, "12 knn-reg value on a 5-duplicate query = 11");
        assert((await kc.predict(Q2))[0] === 0, "12 knn-clf label on the duplicate query = 0");
        kr.close(); kc.close(); Q.close(); Q2.close();
    }

    {
        const m = new GaussianNB();
        await m.fit([[0], [2], [4], [6]], [0, 0, 1, 1]);
        nearArr(await m.predictProba([[3]]), [[0.5, 0.5]], 1e-12, "12 nb proba at 3 = [0.5, 0.5]");
        nearArr(await m.predictProba([[1]]), [[0.9996646498668516, 0.0003353501331483797]], 1e-9,
               "12 nb proba at 1 (python softmax)");
        nearArr(await m.predictProba([[5]]), [[0.0003353501331483797, 0.9996646498668516]], 1e-9,
               "12 nb proba at 5 (python softmax)");
        assert((await m.predict([[5]]))[0] === 1, "12 nb label at 5 = 1");
        const Q = dupQuery([5], 4);
        nearArr(await m.predictProba(Q), [[0.0003353501331483797, 0.9996646498668516]], 1e-9,
               "12 nb proba on a 4-duplicate query reading 5");
        m.close(); Q.close();
    }

    {
        const m = new SVC({ kernel: "linear", C: 100 });
        await m.fit([[-2], [-1], [1], [2]], [0, 0, 1, 1]);
        nearArr(await m.decisionFunction([[-2], [-1], [0], [1], [2]]),
                [-2, -1, 0, 1, 2], 0.05, "12 svc hard-margin decisionFunction = x");
        assert((await m.predict([[-3]]))[0] === 0 && (await m.predict([[3]]))[0] === 1,
               "12 svc labels at the extremes");
        const Q = dupQuery([2], 3);
        nearArr(await m.decisionFunction(Q), [2], 0.05,
               "12 svc decisionFunction on a 3-duplicate query reading 2 = 2");
        m.close(); Q.close();
    }

    {
        const m = new GaussianMixture(2, { seed: 7, regCovar: 1e-6 });
        await m.fit([[-3], [-2.5], [-2], [2], [2.5], [3]]);
        sameSort(m.means, [-2.5, 2.5], 1e-9, "12 gmm means = +-2.5");
        sameSort(m.variances, [1 / 6 + 1e-6, 1 / 6 + 1e-6], 1e-9,
                 "12 gmm variances = 1/6 + regCovar");
        sameSort(m.weights, [0.5, 0.5], 1e-9, "12 gmm weights = 1/2");
        nearArr(await m.predictProba([[0]]), [[0.5, 0.5]], 1e-9,
               "12 gmm resp at 0 = [0.5, 0.5]");
        sameSort(await m.predictProba([[1]]), [9.359307482468292e-14, 0.9999999999999063], 1e-9,
                 "12 gmm resp at 1 (python softmax)");
        assert((await m.predict([[1]]))[0] !== (await m.predict([[-1]]))[0],
               "12 gmm predict splits the halves");
        const Q = dupQuery([0.25], 4);
        sameSort(await m.predictProba(Q), [0.0005528034986214661, 0.9994471965013786], 1e-9,
                 "12 gmm resp on a 4-duplicate query reading 0.25");
        m.close(); Q.close();
    }

    {
        const m = new KMeans(2, 11);
        await m.fit([[0, 0], [0, 0.1], [10, 10], [10, 10.1]]);
        close(m.inertia, 0.01, 1e-9, "12 kmeans forced-blob inertia = 0.01");
        const a = (await m.predict([[0, 0.02]]))[0], b = (await m.predict([[10.02, 10.02]]))[0];
        assert(a === (await m.predict([[0, 0]]))[0] && b === (await m.predict([[10, 10.1]]))[0] &&
               a !== b, "12 kmeans predict agrees within a blob and splits the blobs");
        const Q = dupQuery([4, 10.05], 3);
        assert((await m.predict(Q))[0] === b, "12 kmeans predict on a 3-duplicate query");
        m.close(); Q.close();
    }

    {
        const dtr = await new DecisionTreeRegressor({ maxDepth: 1 })
            .fit([[0], [1], [2], [3]], [0, 0, 10, 10]);
        nearArr(await dtr.predict([[0.5], [2.5]]), [0, 10], 0,
                "12 dtr stump leaf values = 0 and 10 exactly");
        const dtc = await new DecisionTreeClassifier({ maxDepth: 1 })
            .fit([[0], [1], [2], [3]], [0, 0, 1, 1]);
        nearArr(await dtc.predictProba([[0.5]]), [[1, 0]], 0, "12 dtc pure-leaf proba = [1, 0]");
        nearArr(await dtc.predictProba([[2.5]]), [[0, 1]], 0, "12 dtc pure-leaf proba = [0, 1]");
        const Q = dupQuery([2.5], 4);
        nearArr(await dtr.predict(Q), [10], 0, "12 dtr value on a 4-duplicate query reading 2.5");
        dtr.close(); dtc.close(); Q.close();
    }
    {
        const Xc = [[0], [1], [2], [3]], yc = [4.25, 4.25, 4.25, 4.25];
        const rfr = await new RandomForestRegressor({ nEstimators: 7, seed: 2 }).fit(Xc, yc);
        const gbr = await new GradientBoostingRegressor({ nEstimators: 5, seed: 3 }).fit(Xc, yc);
        const xgbr = await new XGBRegressor({ nEstimators: 5, seed: 4 }).fit(Xc, yc);
        const Q = dupQuery([1.5], 4);
        for (const [name, m] of [["RandomForest", rfr], ["GradientBoosting", gbr],
                                 ["XGB", xgbr]]) {
            nearArr(await m.predict([[0], [1.5], [3]]), [4.25, 4.25, 4.25], 1e-12,
                    "12 " + name + " constant-target predict = 4.25");
            assert((await m.predict(Q))[0] === 4.25,
                   "12 " + name + " constant-target predict on a 4-duplicate query = 4.25");
        }
        rfr.close(); gbr.close(); xgbr.close(); Q.close();
    }
}

print("test_ml_sparse: all " + n + " assertions passed");
