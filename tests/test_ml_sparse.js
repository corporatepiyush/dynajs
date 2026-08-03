/* test_ml_sparse.js -- class CSR (bench/ML_PRODUCTION_PLAN.md gap 9).
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
 *      to avoid.
 */
import { CSR, LinearRegression, LogisticRegression, KMeans, PCA, GaussianNB, DecisionTreeClassifier, RandomForestRegressor, XGBRegressor, StandardScaler, accuracy } from "dyna:ml";

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

    /* A negative zero is a zero and is dropped; a NaN is not and is kept. */
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

print("test_ml_sparse: all " + n + " assertions passed");
