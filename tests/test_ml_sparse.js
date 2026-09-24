/* test_ml_sparse.js -- class CSR (the sparse-matrix gap).
 *
 * The gap was not speed. Every fit took a dense matrix, so data that is 99%
 * zeros -- one-hot categories, bag-of-words, interaction terms -- had to be
 * materialised dense before it could be fitted at all. A 100k x 50k one-hot
 * design is 40 GB dense and 40 MB sparse.
 *
 * So the properties that matter are:
 *
 *   1. A sparse fit and a dense fit of the SAME data are the same model. Not
 *      close: the sparse accumulation skips terms that are exactly zero, and
 *      adding zero changes nothing, so the only difference is summation order.
 *   2. A malformed CSR is refused rather than believed. A column index outside
 *      the matrix is an out-of-bounds WRITE into the coefficient vector, not a
 *      wrong answer, so every field is validated at construction.
 *   3. An estimator with no sparse path REFUSES a CSR and names toDense().
 *      Densifying behind the caller's back is the 40 GB the sparse form exists
 *      to avoid. Since the refusals are the fits beyond the two linear
 *      models, apply() and the scaler/PCA transforms; EVERY predict and
 *      predictProba takes a CSR (section 9), one row expanded at a time into
 *      the same kernel the dense form runs.
 */
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
function throws(fn, kind, m) {
    n++;
    try { fn(); } catch (e) {
        if (kind && !(e instanceof kind)) throw new Error((m || "wrong error") + ": " + e);
        return String(e.message || e);
    }
    throw new Error((m || "expected a throw") + " but none happened");
}
function lcg(seed) { let s = seed >>> 0; return () => (s = (s * 1664525 + 1013904223) >>> 0) / 4294967296; }

/* Genuinely sparse data: 4 nonzeros in 60 columns, and the target depends on
 * three of them, so a model that ignored the structure would still have to
 * find the same coefficients. */
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

/* ==================================================================== *
 *  1. The representation itself
 * ==================================================================== */
{
    /* [[1,0,2,0],[0,3,0,0]] written by hand, so the layout is pinned rather
     * than inferred from fromDense's output. */
    const m = new CSR([1, 2, 3], [0, 2, 1], [0, 2, 3], 4);
    assert(m.rows === 2, "rows come from the pointer array");
    assert(m.cols === 4, "cols are given");
    assert(m.nnz === 3, "nnz is the value count");
    close(m.density, 3 / 8, 0, "density");
    assert(JSON.stringify(m.row(0)) === "[1,0,2,0]", "row 0");
    assert(JSON.stringify(m.row(1)) === "[0,3,0,0]", "row 1");
    assert(JSON.stringify(m.toDense()) === "[[1,0,2,0],[0,3,0,0]]", "toDense");

    /* An empty row is legal and common -- a document with no known words. */
    const e = new CSR([5], [1], [0, 0, 1], 3);
    assert(JSON.stringify(e.row(0)) === "[0,0,0]", "an empty row is all zeros");
    assert(JSON.stringify(e.row(1)) === "[0,5,0]", "and the next row is not");

    /* A matrix with no nonzeros at all. */
    const z = new CSR([], [], [0, 0, 0], 2);
    assert(z.nnz === 0 && z.rows === 2, "an all-zero matrix is representable");
    close(z.density, 0, 0, "and has zero density");
    m.close(); e.close(); z.close();
}

/* ==================================================================== *
 *  2. fromDense round-trips exactly
 * ==================================================================== */
{
    const d = sparseData(9, 200, 40, 3);
    const S = CSR.fromDense(d.dense);
    assert(S.rows === 200 && S.cols === 40, "shape survives");
    assert(S.nnz <= 200 * 3, "no more nonzeros than were written");
    const back = S.toDense();
    let exact = true;
    for (let i = 0; i < 200; i++)
        for (let j = 0; j < 40; j++)
            if (back[i][j] !== d.dense[i][j]) exact = false;
    assert(exact, "toDense(fromDense(X)) is X, bit for bit");
    for (let i = 0; i < 5; i++)
        assert(S.row(i).every((v, j) => v === d.dense[i][j]), "row " + i);

    /* A negative zero is a zero and is dropped. A NaN cannot arrive at all:
     * both constructors refuse a non-finite value (test_ml_fit_parity). */
    const odd = CSR.fromDense([[0, -0, 1e-300]]);
    assert(odd.nnz === 1, "-0 is a zero: " + odd.nnz);
    S.close(); odd.close();
}

/* ==================================================================== *
 *  3. THE EQUALITY: a sparse fit is the dense fit
 * ==================================================================== */
{
    for (const [rows, cols, nz] of [[400, 60, 4], [150, 20, 2], [60, 200, 3]]) {
        const d = sparseData(11 + cols, rows, cols, nz);
        const S = CSR.fromDense(d.dense);
        const tag = rows + "x" + cols;

        const a = new LinearRegression().fit(d.dense, d.y);
        const b = new LinearRegression().fit(S, d.y);
        const pa = a.predict(d.dense), pb = b.predict(d.dense);
        let worst = 0;
        for (let i = 0; i < pa.length; i++) worst = Math.max(worst, Math.abs(pa[i] - pb[i]));
        close(worst, 0, 1e-9, tag + ": LinearRegression sparse equals dense");

        const c = new LogisticRegression({ maxIter: 200 }).fit(d.dense, d.yc);
        const e = new LogisticRegression({ maxIter: 200 }).fit(S, d.yc);
        const qa = c.predictProba(d.dense).map(r => r[1]);
        const qb = e.predictProba(d.dense).map(r => r[1]);
        let w2 = 0;
        for (let i = 0; i < qa.length; i++) w2 = Math.max(w2, Math.abs(qa[i] - qb[i]));
        close(w2, 0, 1e-9, tag + ": LogisticRegression sparse equals dense");
        a.close(); b.close(); c.close(); e.close(); S.close();
    }
}

/* ==================================================================== *
 *  4. Sparse fits compose with everything else the fit takes
 * ==================================================================== */
{
    const d = sparseData(21, 300, 30, 3);
    const S = CSR.fromDense(d.dense);
    const w = d.y.map((_, i) => (i % 2 ? 2 : 0.5));

    const a = new LinearRegression().fit(d.dense, d.y, { sampleWeight: w });
    const b = new LinearRegression().fit(S, d.y, { sampleWeight: w });
    const pa = a.predict(d.dense), pb = b.predict(d.dense);
    let worst = 0;
    for (let i = 0; i < pa.length; i++) worst = Math.max(worst, Math.abs(pa[i] - pb[i]));
    close(worst, 0, 1e-9, "sampleWeight works on the sparse path too");

    /* Multiclass logistic over a sparse design. */
    const y3 = d.dense.map(r => (r[0] > 0.3 ? 2 : (r[1] > 0.3 ? 1 : 0)));
    const c = new LogisticRegression({ maxIter: 200 }).fit(d.dense, y3);
    const e = new LogisticRegression({ maxIter: 200 }).fit(S, y3);
    close(accuracy(y3, c.predict(d.dense)), accuracy(y3, e.predict(d.dense)), 1e-12,
          "multiclass sparse equals dense");

    /* A model fitted sparsely predicts on dense rows -- predict has no sparse
     * form, and the coefficients do not know how they were computed. */
    assert(b.predict([d.dense[0]]).length === 1, "a sparsely fitted model predicts densely");
    a.close(); b.close(); c.close(); e.close(); S.close();
}

/* ==================================================================== *
 *  5. Everything else REFUSES, and says what to do
 * ==================================================================== */
{
    const S = CSR.fromDense([[1, 0], [0, 2], [3, 0], [0, 4]]);
    const y = [1, 2, 3, 4], yc = [0, 1, 0, 1];

    const msg = throws(() => new KMeans(2).fit(S), TypeError, "KMeans refuses a CSR");
    assert(msg.includes("toDense"), "and names toDense(): " + msg);

    throws(() => new PCA(1).fit(S), TypeError, "PCA refuses");
    throws(() => new GaussianNB().fit(S, yc), TypeError, "GaussianNB refuses");
    throws(() => new DecisionTreeClassifier().fit(S, yc), TypeError, "the tree refuses");
    throws(() => new RandomForestRegressor({ nEstimators: 2 }).fit(S, y), TypeError,
           "the forest refuses");
    throws(() => new XGBRegressor({ nEstimators: 2 }).fit(S, y), TypeError, "XGB refuses");
    throws(() => new StandardScaler().fit(S), TypeError, "the scaler refuses");

    /* ...and toDense() is the way through, so the refusal is not a dead end. */
    assert(new KMeans(2).fit(S.toDense()).predict(S.toDense()).length === 4,
           "toDense() makes it work");
    S.close();
}

/* ==================================================================== *
 *  6. A malformed CSR is refused at construction
 *
 *  Each of these is an out-of-bounds write rather than a wrong answer if it
 *  gets through, which is why the check is at the boundary and not at use.
 * ==================================================================== */
{
    throws(() => new CSR([1], [9], [0, 1], 4), RangeError, "a column outside the matrix");
    throws(() => new CSR([1], [-1], [0, 1], 4), RangeError, "a negative column");
    throws(() => new CSR([1], [0.5], [0, 1], 4), RangeError, "a fractional column");
    throws(() => new CSR([1], [0], [1, 1], 4), RangeError, "pointers not starting at 0");
    throws(() => new CSR([1], [0], [0, 2], 4), RangeError, "pointers past the value count");
    throws(() => new CSR([1, 2], [0, 1], [0, 2, 1], 4), RangeError, "decreasing pointers");
    throws(() => new CSR([1], [0, 1], [0, 1], 4), TypeError, "columns longer than values");
    throws(() => new CSR([NaN], [0], [0, 1], 4), RangeError, "a NaN value");
    throws(() => new CSR([1], [0], [0, 1], 0), RangeError, "zero columns");
    throws(() => new CSR([1], [0], [], 4), RangeError, "no row pointers");
    throws(() => new CSR([1], [0]), TypeError, "too few arguments");

    /* The error names the offending entry, not just the fact of one. */
    const m = throws(() => new CSR([1, 2, 3], [0, 7, 1], [0, 3], 4), RangeError, "names it");
    assert(m.includes("[1]"), "the message names the index: " + m);
}

/* ==================================================================== *
 *  7. Adversarial arguments
 * ==================================================================== */
{
    const S = CSR.fromDense([[1, 0], [0, 2]]);
    throws(() => S.row(2), RangeError, "row past the end");
    throws(() => S.row(-1), RangeError, "a negative row");

    /* row(i) coerces the index, which runs user JS that can close the matrix.
     * The coercion must happen BEFORE the handle is resolved. */
    let ran = 0;
    const bomb = { valueOf() { ran++; S.close(); return 0; } };
    throws(() => S.row(bomb), Error, "closing during the index coercion");
    assert(ran > 0, "the attack ran: " + ran);

    /* A CSR whose y is the wrong length is a shape error, not a silent fit. */
    const T = CSR.fromDense([[1, 0], [0, 2], [3, 0]]);
    throws(() => new LinearRegression().fit(T, [1, 2]), TypeError, "y too short");
    T.close();
}

/* ==================================================================== *
 *  8. It survives being closed, twice, and used after
 * ==================================================================== */
{
    const S = CSR.fromDense([[1, 0], [0, 2]]);
    S.close();
    S.close();                       /* idempotent */
    n++;
    throws(() => S.row(0), Error, "using a closed CSR throws");
    throws(() => new LinearRegression().fit(S, [1, 2]), Error, "and so does fitting one");
}

/* ==================================================================== *
 *  9. every predict/predictProba takes a CSR (KMeans and friends)
 *
 *  THE EQUALITY again, from the prediction side: for the SAME fitted model,
 *  predict over a CSR and predict over the dense same data are the same
 *  answer. A sparse row is expanded one row at a time into the same kernel
 *  the dense form runs, so this is not a tolerance property -- the two calls
 *  run identical arithmetic on identical rows and must agree EXACTLY.
 * ==================================================================== */
{
    const d = sparseData(31, 120, 12, 3);
    const S = CSR.fromDense(d.dense);
    const Q = CSR.fromDense(d.dense.slice(0, 9));   // a sparse QUERY set too
    const eqOut = (a, b, tag) => {
        const A = Array.from(a), B = Array.from(b);
        assert(A.length === B.length &&
               A.every((v, i) => Object.is(v, B[i])),
               tag + ": " + JSON.stringify(A) + " vs " + JSON.stringify(B));
    };

    const km = new KMeans(3, 5).fit(d.dense);   /* (k, seed): positional */
    eqOut(km.predict(S), km.predict(d.dense), "KMeans.predict");
    const into = new Float64Array(9);
    assert(km.predictInto(into, Q) === 9, "KMeans.predictInto(CSR) writes all rows");
    eqOut(into, km.predict(d.dense).slice(0, 9), "KMeans.predictInto values");

    const dtc = new DecisionTreeClassifier({ seed: 1 }).fit(d.dense, d.yc);
    eqOut(dtc.predict(S), dtc.predict(d.dense), "DecisionTreeClassifier.predict");
    const dtr = new DecisionTreeRegressor({ seed: 1 }).fit(d.dense, d.y);
    eqOut(dtr.predict(S), dtr.predict(d.dense), "DecisionTreeRegressor.predict");

    const rfc = new RandomForestClassifier({ nEstimators: 5, seed: 2 }).fit(d.dense, d.yc);
    eqOut(rfc.predict(S), rfc.predict(d.dense), "RandomForestClassifier.predict");
    const rfr = new RandomForestRegressor({ nEstimators: 5, seed: 2 }).fit(d.dense, d.y);
    eqOut(rfr.predict(S), rfr.predict(d.dense), "RandomForestRegressor.predict");
    const pbS = rfc.predictProba(S), pbD = rfc.predictProba(d.dense);
    assert(Array.isArray(pbS[0]), "a CSR predictProba answers in the row-arrays form");
    assert(pbS.every((r, i) => r.every((v, j) => Object.is(v, pbD[i][j]))),
           "RandomForestClassifier.predictProba: CSR == dense, row for row");

    const gbr = new GradientBoostingRegressor({ nEstimators: 5, seed: 3 }).fit(d.dense, d.y);
    eqOut(gbr.predict(S), gbr.predict(d.dense), "GradientBoostingRegressor.predict");
    const gbc = new GradientBoostingClassifier({ nEstimators: 5, seed: 3 }).fit(d.dense, d.yc);
    eqOut(gbc.predict(S), gbc.predict(d.dense), "GradientBoostingClassifier.predict");
    const xgbr = new XGBRegressor({ nEstimators: 5, seed: 4 }).fit(d.dense, d.y);
    eqOut(xgbr.predict(S), xgbr.predict(d.dense), "XGBRegressor.predict");
    const xgbc = new XGBClassifier({ nEstimators: 5, seed: 4 }).fit(d.dense, d.yc);
    eqOut(xgbc.predict(S), xgbc.predict(d.dense), "XGBClassifier.predict");

    const nb = new GaussianNB().fit(d.dense, d.yc);
    eqOut(nb.predict(S), nb.predict(d.dense), "GaussianNB.predict");
    const nbpS = nb.predictProba(S), nbpD = nb.predictProba(d.dense);
    assert(nbpS.every((r, i) => r.every((v, j) => Object.is(v, nbpD[i][j]))),
           "GaussianNB.predictProba: CSR == dense");

    const kc = new KNClassifier(3).fit(d.dense, d.yc);
    eqOut(kc.predict(S), kc.predict(d.dense), "KNClassifier.predict");
    const kr = new KNRegressor(3, "distance").fit(d.dense, d.y);
    eqOut(kr.predict(Q), kr.predict(d.dense.slice(0, 9)), "KNRegressor.predict");

    const svc = new SVC().fit(d.dense, d.yc);
    eqOut(svc.predict(S), svc.predict(d.dense), "SVC.predict");
    const dfS = svc.decisionFunction(S), dfD = svc.decisionFunction(d.dense);
    eqOut(Array.isArray(dfS) ? dfS.flat() : dfS, Array.isArray(dfD) ? dfD.flat() : dfD,
          "SVC.decisionFunction");

    const gmm = new GaussianMixture(2, { seed: 6 }).fit(d.dense);
    eqOut(gmm.predict(S), gmm.predict(d.dense), "GaussianMixture.predict");
    const gpS = gmm.predictProba(S), gpD = gmm.predictProba(d.dense);
    assert(gpS.every((r, i) => r.every((v, j) => Object.is(v, gpD[i][j]))),
           "GaussianMixture.predictProba: CSR == dense");

    km.close(); dtc.close(); dtr.close(); rfc.close(); rfr.close();
    gbr.close(); gbc.close(); xgbr.close(); xgbc.close(); nb.close();
    kc.close(); kr.close(); svc.close(); gmm.close();
    S.close(); Q.close();
}

/* ==================================================================== *
 *  10. adversarial: what a sparse predict must still refuse
 *
 *  The refusal surface is part of the contract: which form X arrived in
 *  must not change what predict refuses (NaN rows, wrong widths, short
 *  out buffers, closed handles), and the duplicate-index rule must be the
 *  SAME through a predict as it is through toDense().
 * ==================================================================== */
{
    const d = sparseData(41, 40, 6, 2);
    const S = CSR.fromDense(d.dense);
    const kc = new KNClassifier(3).fit(d.dense, d.yc);

    /* a non-finite value cannot even be CONSTRUCTED -- the ctor and fromDense
     * refuse one identically -- and the one way a finite-valued matrix reads
     * non-finite, a duplicate-SUM overflow, is refused by a predict exactly
     * like its dense reading */
    throws(() => new CSR([1, NaN], [0, 1], [0, 2], 6), RangeError,
           "a NaN value is refused at construction");
    throws(() => CSR.fromDense([[1, NaN, 0, 0, 0, 0]]), RangeError,
           "a NaN value is refused by fromDense too");
    const N = new CSR([1e308, 1e308], [0, 0], [0, 2], 6);
    const msg = throws(() => kc.predict(N), RangeError, "an overflow-class row is refused");
    assert(/NaN or infinite/.test(msg), "the refusal names the values: " + msg);

    /* a wrong width is a shape error before any row is walked */
    const W = CSR.fromDense([[1, 0]]);
    throws(() => kc.predict(W), TypeError, "a CSR with the wrong width");

    /* a short out is refused BEFORE anything is written */
    const out = new Float64Array(2);
    throws(() => kc.predictInto(out, S), RangeError, "a short out buffer");
    assert(out[0] === 0, "and nothing was written");

    /* predict before fit, and a closed handle, refuse as ever */
    throws(() => new KNClassifier(3).predict(S), Error, "predict before fit");
    const C = CSR.fromDense([[1, 0, 0, 0, 0, 0]]);
    C.close();
    throws(() => kc.predict(C), Error, "a closed CSR");

    /* empty shapes: no rows predicts no rows */
    const E = new CSR([], [], [0], 6);
    assert(kc.predict(E).length === 0, "an empty CSR predicts no rows");
    const Z = new CSR([], [], [0, 0], 6);          // one EMPTY row
    assert(kc.predict(Z)[0] === kc.predict([[0, 0, 0, 0, 0, 0]])[0],
           "an empty row is an all-zero row");

    /* THE RULE: duplicate column indices SUM (scipy), and every walk of the
     * matrix agrees -- row(), toDense() and a predict all read [5,0,...] out
     * of two entries at column 0. */
    const dup = new CSR([2, 3], [0, 0], [0, 2], 6);
    assert(dup.toDense()[0][0] === 5 && dup.row(0)[0] === 5,
           "toDense/row sum duplicate indices");
    assert(kc.predict(dup)[0] === kc.predict(dup.toDense())[0],
           "predict reads duplicates the same way toDense does");
    const uns = new CSR([1, 1], [5, 0], [0, 2], 6);  // unsorted within a row
    assert(kc.predict(uns)[0] === kc.predict(uns.toDense())[0],
           "column order within a row is irrelevant");

    kc.close(); S.close(); N.close(); W.close(); E.close(); Z.close();
    dup.close(); uns.close();
}

/* ==================================================================== *
 *  11. duplicate-SUM at VALUE level (the predict path included)
 *
 *  Section 10's duplicate check reads LABELS through KNN, and labels are
 *  piecewise-constant: a dropped duplicate SUM moves a score without moving
 *  the argmax. These probes compare SCORES -- predict values, predictProba
 *  rows, decisionFunction values -- bit for bit (Object.is) between a
 *  duplicate-laden CSR and its dense reading, over 2-5 duplicate indices per
 *  column, and pin row()/toDense() sums exactly. A mutation that drops the
 *  SUM in the predict path changes the scores and fails here.
 * ==================================================================== */
{
    const flat = (a) => Array.from(a).flat(9);
    const same = (a, b) => {
        const A = flat(a), B = flat(b);
        return A.length === B.length && A.every((v, i) => Object.is(v, B[i]));
    };
    /* Split v into k duplicate entries whose STORAGE-ORDER sum is bitwise v:
     * two leading v/2^(k-1) then v/2^(k-2)...v/2, so every partial sum doubles
     * the last -- exact in IEEE binary64. A predict that drops the SUM keeps
     * only v/2 or only v/2^(k-1), a value change no tolerance can hide. */
    const dupSplit = (v, k) => {
        const out = [v / Math.pow(2, k - 1), v / Math.pow(2, k - 1)];
        for (let j = k - 2; j >= 1; j--) out.push(v / Math.pow(2, j));
        return out;
    };
    /* one row whose columns are 2..5-duplicate stacks summing to `reading` */
    const dupRow = (reading, k) => {
        const val = [], col = [], ptr = [0];
        for (let c = 0; c < reading.length; c++) {
            const parts = dupSplit(reading[c], ((c % (k - 1)) + 2));
            for (const p of parts) { val.push(p); col.push(c); }
        }
        ptr.push(val.length);
        return new CSR(val, col, ptr, reading.length);
    };

    /* 11a. row()/toDense(): 2-5 duplicate indices sum exactly */
    {
        const S = new CSR([2, 3, 5, 5, 5, 5, 5, 0.5, 0.25, 0.25, 7, -7, 1, 1, 1],
                          [0, 0, 1, 1, 1, 1, 1, 2,    2,    2,    3, 3,  4, 4, 4],
                          [0, 15], 5);
        const r = S.row(0), D = S.toDense()[0];
        assert(same(r, D), "11a row() == toDense() bitwise with 2-5 duplicates");
        assert(Object.is(r[0], 5) && Object.is(r[1], 25) && Object.is(r[2], 1) &&
               Object.is(r[3], 0) && Object.is(r[4], 3),
               "11a 2-5 duplicate sums are exact (5, 25, 1, 0, 3): " + JSON.stringify(r));
        const T = new CSR([1e16, 1], [0, 0], [0, 2], 1);
        const t = T.row(0)[0];
        assert(Object.is(t, 1e16 + 1) || Object.is(t, 1e16),
               "11a duplicate sum follows storage order: " + t);
        /* the doubling split: bitwise identity to the undivided value */
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

    /* 11b. per-family predict VALUE equality: duplicate-laden CSR vs the dense
     * reading, on scores not labels, through every predict-side family */
    {
        const Xt = [[1, 0, 2], [0, 3, 0], [4, 0, 5], [1, 1, 1], [2, 2, 0], [0, 1, 3]];
        const yt = [1, 2, 3, 2, 2, 3], yr = [1.5, 2.5, 3.5, 2.0, 2.2, 3.1];
        /* query reading [2.3, -1.25, 0.75] as 2-5-duplicate stacks */
        const Q = dupRow([2.3, -1.25, 0.75], 5);
        const QD = Q.toDense();
        const eqVals = (name, fn) =>
            assert(same(fn(Q), fn(QD)), "11b " + name + ": dup-laden CSR == dense, bit for bit");
        const lin = new LinearRegression().fit(Xt, yr);
        const log = new LogisticRegression({ maxIter: 400 }).fit(Xt, yt);
        const kr = new KNRegressor(3).fit(Xt, yr);
        const nb = new GaussianNB().fit(Xt, yt);
        const svc = new SVC().fit(Xt, yt);
        const gmm = new GaussianMixture(2, { seed: 6 }).fit(Xt);
        const km = new KMeans(3, 7).fit(Xt);
        const dtr = new DecisionTreeRegressor({ seed: 1 }).fit(Xt, yr);
        const rfr = new RandomForestRegressor({ nEstimators: 4, seed: 2 }).fit(Xt, yr);
        const gbr = new GradientBoostingRegressor({ nEstimators: 4, seed: 3 }).fit(Xt, yr);
        eqVals("LinearRegression.predict", (X) => lin.predict(X));
        eqVals("LogisticRegression.predict", (X) => log.predict(X));
        eqVals("LogisticRegression.predictProba", (X) => log.predictProba(X));
        eqVals("KNRegressor.predict", (X) => kr.predict(X));
        eqVals("GaussianNB.predictProba", (X) => nb.predictProba(X));
        eqVals("SVC.decisionFunction", (X) => svc.decisionFunction(X));
        eqVals("GaussianMixture.predictProba", (X) => gmm.predictProba(X));
        eqVals("KMeans.predict", (X) => km.predict(X));
        eqVals("DecisionTreeRegressor.predict", (X) => dtr.predict(X));
        eqVals("RandomForestRegressor.predict", (X) => rfr.predict(X));
        eqVals("GradientBoostingRegressor.predict", (X) => gbr.predict(X));
        /* predictInto shares the same rows */
        {
            const a = new Float64Array(1), b = new Float64Array(1);
            lin.predictInto(a, Q);
            lin.predictInto(b, QD);
            assert(Object.is(a[0], b[0]), "11b LinearRegression.predictInto too");
        }
        lin.close(); log.close(); kr.close(); nb.close(); svc.close();
        gmm.close(); km.close(); dtr.close(); rfr.close(); gbr.close();
        Q.close();
    }

    /* 11c. the adversarial FP hunt: duplicate pairs and triples over a value
     * grid that maximises rounding disagreement between "sum then multiply"
     * and "multiply then sum", plus the overflow class */
    {
        const lin = new LinearRegression();
        lin.fit([[1, 0], [0, 1], [1, 1], [2, 1]], [3, 0, 3, 6]);  /* coef ~ [3, 0] */
        const log = new LogisticRegression();
        log.fit([[1, 0], [0, 1], [1, 1], [2, 1], [0, 2]], [0, 1, 0, 0, 1]);
        const W = [1, 1e-16, 1e-17, 3, 7, 1e16, -1e16, 0.1, 1e8, 1e-8, 2, -1, 5];
        let diverge = null, checked = 0;
        const hunt = (fn) => {
            for (const a of W) for (const b of W) {
                const S = new CSR([a, b], [0, 0], [0, 2], 2);
                const D = S.toDense();
                checked++;
                const sp = flat(fn(S)), de = flat(fn(D));
                if (!same(sp, de)) {
                    diverge = { dups: [a, b], sp, de };
                    S.close();
                    return;
                }
                S.close();
            }
            for (const a of W) for (const b of W) for (const c of [1, 1e-16, 3, 1e16, 0.1]) {
                const S = new CSR([a, b, c], [0, 0, 0], [0, 3], 2);
                const D = S.toDense();
                checked++;
                const sp = flat(fn(S)), de = flat(fn(D));
                if (!same(sp, de)) {
                    diverge = { dups: [a, b, c], sp, de };
                    S.close();
                    return;
                }
                S.close();
            }
        };
        hunt((X) => lin.predict(X));
        if (!diverge) hunt((X) => log.predictProba(X));
        if (!diverge) hunt((X) => log.predict(X));
        assert(diverge === null,
               "11c lin/logreg CSR predict bitwise-identical to dense under duplicate FP stress (" +
               checked + " rows)",
               diverge ? "dups " + JSON.stringify(diverge.dups) + " sparse=" +
                         JSON.stringify(diverge.sp) + " dense=" + JSON.stringify(diverge.de) : "");
        /* the OVERFLOW class: [1e300, 1e300, -1e300] reads inf in both forms */
        {
            const S = new CSR([1e300, 1e300, -1e300], [0, 0, 0], [0, 3], 2);
            const D = S.toDense();
            assert(same(lin.predict(S), lin.predict(D)),
                   "11c overflow-grade duplicates agree bitwise (predict=" +
                   lin.predict(S) + ")");
            S.close();
        }
        lin.close(); log.close();
    }

    /* 11d. a huge caller-declared width cannot take the process down: the
     * expansion paths throw the documented RangeError instead */
    {
        const H = new CSR([1.0], [0], [0, 1], Math.pow(2, 40));
        const e1 = throws(() => H.row(0), RangeError, "11d huge-width row() throws");
        assert(/fit in memory/.test(e1), "11d row() names the memory bound: " + e1);
        const e2 = throws(() => H.toDense(), RangeError, "11d huge-width toDense() throws");
        assert(/fit in memory/.test(e2), "11d toDense() names the memory bound: " + e2);
        /* the predict-side scratch has the same bound (here refused first as a
         * width mismatch -- the message must print the FULL width) */
        const lin = new LinearRegression().fit([[1, 0, 0], [0, 1, 0], [0, 0, 1]], [1, 2, 3]);
        const e3 = throws(() => lin.predict(H), TypeError, "11d huge-width predict refuses");
        assert(e3.includes("1099511627776"),
               "11d the width message prints the full 64-bit width: " + e3);
        /* the sparse FIT on 2^40 cols refuses before any huge allocation */
        const fitWide = (() => {
            const S = new CSR([1.0], [0], [0, 1], Math.pow(2, 40));
            const mm = new LinearRegression();
            let r;
            try { mm.fit(S, [1]); r = "ACCEPTED"; } catch (e) { r = e.constructor.name; }
            S.close(); mm.close();
            return r;
        })();
        assert(fitWide === "RangeError",
               "11d sparse fit on 2^40 cols refuses with RangeError: " + fitWide);
        H.close(); lin.close();
    }
}

/* ==================================================================== *
 *  12. pinned VALUES per estimator family -- independent oracles
 *
 *  Every expected number below was computed by python/numpy from closed-form
 *  statistics (or from the model's forced analytic optimum on data chosen so
 *  the optimum IS closed-form) -- never recorded from this engine. Each
 *  family gets at least one pin on a DUPLICATE-expanded query row, so the
 *  duplicate-SUM rule is pinned against absolute values, not just against
 *  this engine's own dense form. See the comment over each case for the
 *  computation.
 * ==================================================================== */
{
    const flat = (a) => Array.from(a).flat(9);
    /* k-duplicate expansion whose storage-order sum is bitwise v (section 11) */
    const dupSplit = (v, k) => {
        const out = [v / Math.pow(2, k - 1), v / Math.pow(2, k - 1)];
        for (let j = k - 2; j >= 1; j--) out.push(v / Math.pow(2, j));
        return out;
    };
    /* query row built entirely of duplicate stacks reading `reading` */
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
        /* order-agnostic: components/classes are not ordered by the spec */
        const G = flat(got).slice().sort((a, b) => a - b);
        const W = flat(want).slice().sort((a, b) => a - b);
        nearArr(G, W, tol, m);
    };

    /* ---- linear: y = 3*x0 + 5*x1 + 7 is solved exactly (numpy lstsq on an
     * exactly determined system); the ridge 1e-9 moves coefs by ~1e-9 */
    {
        const m = new LinearRegression();
        m.fit([[1, 0], [0, 1], [1, 1], [2, 1]], [10, 12, 15, 18]);
        nearArr(m.coef, [3, 5], 1e-6, "12 lin coefs = (3, 5)");
        close(m.intercept, 7, 1e-6, "12 lin intercept = 7");
        nearArr(m.predict([[1, 2], [2, 0]]), [20, 13], 1e-6,
               "12 lin predict = 3x0+5x1+7 (numpy arithmetic)");
        const Q = dupQuery([1, 2], 4);          /* reads [1, 2] from 4-dups */
        nearArr(m.predict(Q), [20], 1e-6, "12 lin predict on a 4-duplicate query = 20");
        m.close(); Q.close();
    }

    /* ---- logistic: x=[-2,-1,1,2], y=[0,1,0,1] is symmetric under
     * (x, y) -> (-x, 1-y), so the (unique) optimum has intercept 0 and
     * predictProba(0) = [0.5, 0.5]; p(x) + p(-x) = 1 (sigmoid symmetry).
     * python: sigmoid(0) = 0.5 exactly. */
    {
        const m = new LogisticRegression();
        m.fit([[-2], [-1], [1], [2]], [0, 1, 0, 1]);
        nearArr(m.predictProba([[0]]), [[0.5, 0.5]], 1e-9,
               "12 logreg symmetric proba at 0 = [0.5, 0.5]");
        for (const x of [0.5, 1, 2]) {
            const p = m.predictProba([[x], [-x]]);
            close(p[0][1] + p[1][1], 1, 1e-9, "12 logreg p(" + x + ")+p(-" + x + ")=1");
        }
        /* a duplicate query reading x = 0: [1.5, -1.5] and [0.25, 0.25, -0.5] */
        const Q = new CSR([1.5, -1.5], [0, 0], [0, 2], 1);
        nearArr(m.predictProba(Q), [[0.5, 0.5]], 1e-9,
               "12 logreg proba on a duplicate row reading 0 = [0.5, 0.5]");
        assert(m.predict([[-2]])[0] === 0 && m.predict([[2]])[0] === 1,
               "12 logreg labels follow the (w > 0) optimum");
        m.close(); Q.close();
    }

    /* ---- KNN: brute force, no fit computation. X=[-3,-1,0,2.5,5],
     * y=[1,2,3,10,20], k=3 uniform: the 3 nearest to 3.5 are 2.5, 5, 0 ->
     * mean(10, 20, 3) = 11 (python: (10+20+3)/3 = 11.0). The query is chosen
     * so the dropped-SUM reading v/2 = 1.75 lands among different neighbours
     * (mean 5) -- a mutation of the rule cannot hide here. Classifier
     * y=[0,0,1,1,1]: at -0.4 the neighbours 0, -1, -3 vote [1, 0, 0] -> 0,
     * while -0.2 would vote [1, 0, 1] -> 1. */
    {
        const X = [[-3], [-1], [0], [2.5], [5]];
        const kr = new KNRegressor(3).fit(X, [1, 2, 3, 10, 20]);
        const kc = new KNClassifier(3).fit(X, [0, 0, 1, 1, 1]);
        nearArr(kr.predict([[3.5]]), [11], 0, "12 knn-reg k=3 uniform = 11 exactly");
        assert(kc.predict([[-0.4]])[0] === 0, "12 knn-clf majority = 0");
        const Q = dupQuery([3.5], 5), Q2 = dupQuery([-0.4], 5);
        nearArr(kr.predict(Q), [11], 0, "12 knn-reg value on a 5-duplicate query = 11");
        assert(kc.predict(Q2)[0] === 0, "12 knn-clf label on the duplicate query = 0");
        kr.close(); kc.close(); Q.close(); Q2.close();
    }

    /* ---- GaussianNB: class 0 = {0, 2} (mu=1), class 1 = {4, 6} (mu=5),
     * both biased var = 1, priors 1/2, smoothing eps = 1e-9 * maxvar(1).
     * score = log(1/2) - 0.5*(log(2*pi*v) + (x-mu)^2/v), v = 1 + 1e-9,
     * proba = softmax over scores. python pins below. */
    {
        const m = new GaussianNB();
        m.fit([[0], [2], [4], [6]], [0, 0, 1, 1]);
        nearArr(m.predictProba([[3]]), [[0.5, 0.5]], 1e-12, "12 nb proba at 3 = [0.5, 0.5]");
        nearArr(m.predictProba([[1]]), [[0.9996646498668516, 0.0003353501331483797]], 1e-9,
               "12 nb proba at 1 (python softmax)");
        nearArr(m.predictProba([[5]]), [[0.0003353501331483797, 0.9996646498668516]], 1e-9,
               "12 nb proba at 5 (python softmax)");
        assert(m.predict([[5]])[0] === 1, "12 nb label at 5 = 1");
        const Q = dupQuery([5], 4);
        nearArr(m.predictProba(Q), [[0.0003353501331483797, 0.9996646498668516]], 1e-9,
               "12 nb proba on a 4-duplicate query reading 5");
        m.close(); Q.close();
    }

    /* ---- SVC: hard-margin linear SVM on x=[-2,-1,1,2], y=[0,0,1,1]. The
     * hard-margin primal is strictly convex with the unique optimum touching
     * the margin at x=+-1: f(x) = w*x + b = 1*x + 0 (f(1)=1, f(-1)=-1), so
     * f(-2..2) = [-2,-1,0,1,2]. python: linear solve of the two support
     * equations. tol 0.05 covers the SMO stopping tolerance (1e-3). */
    {
        const m = new SVC({ kernel: "linear", C: 100 });
        m.fit([[-2], [-1], [1], [2]], [0, 0, 1, 1]);
        nearArr(m.decisionFunction([[-2], [-1], [0], [1], [2]]),
                [-2, -1, 0, 1, 2], 0.05, "12 svc hard-margin decisionFunction = x");
        assert(m.predict([[-3]])[0] === 0 && m.predict([[3]])[0] === 1,
               "12 svc labels at the extremes");
        const Q = dupQuery([2], 3);
        nearArr(m.decisionFunction(Q), [2], 0.05,
               "12 svc decisionFunction on a 3-duplicate query reading 2 = 2");
        m.close(); Q.close();
    }

    /* ---- GaussianMixture: k=2 on [-3,-2.5,-2, 2,2.5,3], regCovar=1e-6. EM
     * converges to mu = -+2.5 (the sample means of each half -- forced by the
     * 12-sigma separation), biased var = ((0.5^2+0+0.5^2)/3) + 1e-6 =
     * 1/6 + 1e-6, weights 1/2. resp = softmax(log w - 0.5*log(2*pi*var) -
     * 0.5*(x-mu)^2/var). python pins below (component order is not part of
     * the spec, so values match order-agnostically). */
    {
        const m = new GaussianMixture(2, { seed: 7, regCovar: 1e-6 });
        m.fit([[-3], [-2.5], [-2], [2], [2.5], [3]]);
        sameSort(m.means, [-2.5, 2.5], 1e-9, "12 gmm means = +-2.5");
        sameSort(m.variances, [1 / 6 + 1e-6, 1 / 6 + 1e-6], 1e-9,
                 "12 gmm variances = 1/6 + regCovar");
        sameSort(m.weights, [0.5, 0.5], 1e-9, "12 gmm weights = 1/2");
        nearArr(m.predictProba([[0]]), [[0.5, 0.5]], 1e-9,
               "12 gmm resp at 0 = [0.5, 0.5]");
        sameSort(m.predictProba([[1]]), [9.359307482468292e-14, 0.9999999999999063], 1e-9,
                 "12 gmm resp at 1 (python softmax)");
        assert(m.predict([[1]])[0] !== m.predict([[-1]])[0],
               "12 gmm predict splits the halves");
        /* the duplicate query reads 0.25, whose resp is python-pinned well
         * away from saturation; the dropped-SUM reading 0.125 answers
         * [0.023, 0.977] instead -- far outside the 1e-9 bound */
        const Q = dupQuery([0.25], 4);
        sameSort(m.predictProba(Q), [0.0005528034986214661, 0.9994471965013786], 1e-9,
                 "12 gmm resp on a 4-duplicate query reading 0.25");
        m.close(); Q.close();
    }

    /* ---- KMeans: blobs [0,0]-[0,0.1] and [10,10]-[10,10.1] force the
     * 2-means partition (centroids [0,0.05] and [10,10.05]); inertia is then
     * 4 * 0.05^2 = 0.01. python: 4*0.05**2 = 0.01. */
    {
        const m = new KMeans(2, 11);
        m.fit([[0, 0], [0, 0.1], [10, 10], [10, 10.1]]);
        close(m.inertia, 0.01, 1e-9, "12 kmeans forced-blob inertia = 0.01");
        const a = m.predict([[0, 0.02]])[0], b = m.predict([[10.02, 10.02]])[0];
        assert(a === m.predict([[0, 0]])[0] && b === m.predict([[10, 10.1]])[0] &&
               a !== b, "12 kmeans predict agrees within a blob and splits the blobs");
        /* the duplicate query reads [4, 10.05] (deep in blob B); the dropped-
         * SUM reading [2, 5.025] is deep in blob A instead */
        const Q = dupQuery([4, 10.05], 3);
        assert(m.predict(Q)[0] === b, "12 kmeans predict on a 3-duplicate query");
        m.close(); Q.close();
    }

    /* ---- tree family. A maxDepth-1 CART on X=[0,1,2,3], y=[0,0,10,10]
     * splits once between 1 and 2 and the leaves are the pure means 0 and 10
     * (closed form). The classifier version answers pure [1, 0]/[0, 1].
     * A forest/boosting on a CONSTANT target y = 4.25 must answer 4.25
     * EXACTLY whatever its bootstrap or leaf structure: every leaf mean is
     * 4.25 and the base score is the mean of y (python: mean([4.25]*n) = 4.25). */
    {
        const dtr = new DecisionTreeRegressor({ maxDepth: 1 })
            .fit([[0], [1], [2], [3]], [0, 0, 10, 10]);
        nearArr(dtr.predict([[0.5], [2.5]]), [0, 10], 0,
                "12 dtr stump leaf values = 0 and 10 exactly");
        const dtc = new DecisionTreeClassifier({ maxDepth: 1 })
            .fit([[0], [1], [2], [3]], [0, 0, 1, 1]);
        nearArr(dtc.predictProba([[0.5]]), [[1, 0]], 0, "12 dtc pure-leaf proba = [1, 0]");
        nearArr(dtc.predictProba([[2.5]]), [[0, 1]], 0, "12 dtc pure-leaf proba = [0, 1]");
        const Q = dupQuery([2.5], 4);
        nearArr(dtr.predict(Q), [10], 0, "12 dtr value on a 4-duplicate query reading 2.5");
        dtr.close(); dtc.close(); Q.close();
    }
    {
        const Xc = [[0], [1], [2], [3]], yc = [4.25, 4.25, 4.25, 4.25];
        const rfr = new RandomForestRegressor({ nEstimators: 7, seed: 2 }).fit(Xc, yc);
        const gbr = new GradientBoostingRegressor({ nEstimators: 5, seed: 3 }).fit(Xc, yc);
        const xgbr = new XGBRegressor({ nEstimators: 5, seed: 4 }).fit(Xc, yc);
        const Q = dupQuery([1.5], 4);
        for (const [name, m] of [["RandomForest", rfr], ["GradientBoosting", gbr],
                                 ["XGB", xgbr]]) {
            nearArr(m.predict([[0], [1.5], [3]]), [4.25, 4.25, 4.25], 1e-12,
                    "12 " + name + " constant-target predict = 4.25");
            assert(m.predict(Q)[0] === 4.25,
                   "12 " + name + " constant-target predict on a 4-duplicate query = 4.25");
        }
        rfr.close(); gbr.close(); xgbr.close(); Q.close();
    }
}

print("test_ml_sparse: all " + n + " assertions passed");
