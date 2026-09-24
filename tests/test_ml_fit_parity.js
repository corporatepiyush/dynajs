/* test_ml_fit_parity.js -- the FIT sees ONE matrix, whichever form it arrived
 * in, and the expansion guard keeps its promises at the boundary.
 *
 * These rows exist because the LinearRegression sparse fit used to consume the
 * STORED nonzero pairs, assuming one entry per column in ascending order. Both
 * assumptions are false of constructor-legal CSRs: duplicate column indices
 * (which row()/toDense()/every predict SUM) and within-row-unsorted column
 * indices (which every predict treats as order-free) silently fitted a
 * DIFFERENT matrix -- a forced optimum y=3x0+5x1+7 came back (10.29, 9.14,
 * 19.43) from a duplicate-expanded row and (-3, -4.17, -3.5) from an unsorted
 * one. The fit now consumes every row through its READING -- duplicates summed
 * in storage order, unsorted rows sorted -- with the same rounding as the dense
 * arm, so a sparse fit is BIT-EQUAL to the dense fit of the same matrix and
 * these rows pin that with Object.is (no tolerance exists to hide behind).
 *
 * Also pinned here:
 *   - the finite-policy parity edge: a row whose STORED values are all finite
 *     but whose duplicate SUM overflows must be refused wherever its dense
 *     reading is refused, and accepted wherever it is accepted (both
 *     directions, every family, predict and fit alike);
 *   - the constructor symmetry: new CSR(...) and CSR.fromDense(...) refuse a
 *     non-finite value identically, so a CSR can never carry one;
 *   - the "does not fit in memory" boundary: the admitted maximum completes
 *     and one past it refuses with the documented RangeError -- at the exact
 *     measured guard formula -- with no SIGSEGV in either direction.
 */
import { CSR, LinearRegression, LogisticRegression, KMeans, GaussianNB,
         DecisionTreeClassifier, DecisionTreeRegressor, RandomForestClassifier,
         RandomForestRegressor, GradientBoostingClassifier, GradientBoostingRegressor,
         XGBClassifier, XGBRegressor, KNClassifier, KNRegressor, SVC,
         GaussianMixture } from "dyna:ml";

let n = 0;
function assert(c, m) { n++; if (!c) throw new Error("assertion failed: " + m); }
function throws(fn, kind, m) {
    n++;
    try { fn(); } catch (e) {
        if (kind && !(e instanceof kind)) throw new Error((m || "wrong error") + ": " + e);
        return String(e.message || e);
    }
    throw new Error((m || "expected a throw") + " but none happened");
}
const flat = (a) => Array.from(a).flat(9);
const same = (a, b) => {
    const A = flat(a), B = flat(b);
    return A.length === B.length && A.every((v, i) => Object.is(v, B[i]));
};
function lcg(seed) { let s = seed >>> 0; return () => (s = (s * 1664525 + 1013904223) >>> 0) / 4294967296; }

/* k entries whose STORAGE-ORDER sum is bitwise v (exact doubling) */
function dupSplit(v, k) {
    const out = [v / Math.pow(2, k - 1), v / Math.pow(2, k - 1)];
    for (let j = k - 2; j >= 1; j--) out.push(v / Math.pow(2, j));
    return out;
}

/* Every constructor-legal form of the SAME matrix, all readings == X bitwise.
 * dupX splits each nonzero into exact duplicate stacks, unX reverses each
 * row's entries, unDupX does both, mixX scatters 2-3 way duplicates through
 * reversed rows. */
function forms(X) {
    const out = [];
    out.push(["fromDense", CSR.fromDense(X)]);
    const build = (emit) => {
        const val = [], col = [], ptr = [0];
        for (const r of X) { emit(r, val, col); ptr.push(val.length); }
        return new CSR(val, col, ptr, X[0].length);
    };
    out.push(["dup2", build((r, val, col) => {
        for (let c = 0; c < r.length; c++)
            if (r[c] !== 0) { val.push(r[c] / 2, r[c] / 2); col.push(c, c); }
    })]);
    out.push(["dup3", build((r, val, col) => {
        for (let c = 0; c < r.length; c++)
            if (r[c] !== 0)
                for (const p of dupSplit(r[c], 1 + (c % 3) + 1)) { val.push(p); col.push(c); }
    })]);
    out.push(["unsorted", build((r, val, col) => {
        const nz = [];
        for (let c = 0; c < r.length; c++) if (r[c] !== 0) nz.push([c, r[c]]);
        nz.reverse();
        for (const [c, v] of nz) { val.push(v); col.push(c); }
    })]);
    out.push(["unsortedDup", build((r, val, col) => {
        const nz = [];
        for (let c = 0; c < r.length; c++) if (r[c] !== 0) nz.push([c, r[c]]);
        nz.reverse();
        for (const [c, v] of nz) {
            const parts = dupSplit(v, 2 + (c % 3));
            for (const p of parts) { val.push(p); col.push(c); }
        }
    })]);
    return out;
}

/* ==================================================================== *
 *  1. forced-optimum systems: every storage form fits the SAME model
 *
 *  y = Xw with X carrying a unit-vector block (so the optimum is exactly w
 *  and the system is well conditioned) plus sparse noise rows. Every form of
 *  X -- sorted, duplicated, unsorted, duplicated+unsorted -- must fit
 *  bit-identically to the dense fit AND recover w through the 1e-9 ridge.
 * ==================================================================== */
for (const seed of [1, 2, 3]) {
    for (const [rows, cols] of [[4, 2], [5, 3], [12, 5], [8, 3]]) {
        const rnd = lcg(seed * 100 + cols);
        const w = [];
        for (let j = 0; j < cols; j++) w.push(((seed * 7 + j * 13) % 17) - 8 + 0.5 * (j % 2));
        const X = [];
        for (let j = 0; j < cols; j++) {           /* the unit-vector block */
            const r = new Array(cols).fill(0);
            r[j] = 1;
            X.push(r);
        }
        while (X.length < rows) {                  /* sparse extra rows */
            const r = new Array(cols).fill(0);
            for (let j = 0; j < cols; j++)
                if (rnd() < 0.5) {
                    const v = Math.round((rnd() * 4 - 2) * 2) / 2;
                    r[j] = v === 0 ? 0 : v;        /* -0 is a zero: collapsed */
                }
            X.push(r);
        }
        const y = X.map(r => r.reduce((a, v, j) => a + v * w[j], 0));
        const grid = [];
        for (let k = 0; k < 4; k++) {
            const r = [];
            for (let j = 0; j < cols; j++) r.push(Math.round((rnd() * 4 - 2) * 2) / 2);
            grid.push(r);
        }
        const md = new LinearRegression().fit(X, y);
        const pd = md.predict(grid);
        for (let j = 0; j < cols; j++)
            assert(Math.abs(md.coef[j] - w[j]) < 1e-6,
                   "1 dense fit recovers w[" + j + "]=" + w[j] + " got " + md.coef[j] +
                   " (" + rows + "x" + cols + " seed " + seed + ")");
        for (const [tag, S] of forms(X)) {
            const rd = S.toDense();
            assert(same(rd, X), "1 setup " + tag + ": reading == X bitwise (" + rows + "x" + cols + ")");
            const m = new LinearRegression().fit(S, y);
            assert(same(m.coef, md.coef),
                   "1 " + tag + " fit coefs == dense fit bitwise (" + rows + "x" + cols +
                   " seed " + seed + ") " + JSON.stringify(m.coef) + " vs " + JSON.stringify(md.coef));
            assert(Object.is(m.intercept, md.intercept),
                   "1 " + tag + " fit intercept == dense fit bitwise (" + rows + "x" + cols + ")");
            assert(same(m.predict(grid), pd),
                   "1 " + tag + " fit predictions == dense fit bitwise (" + rows + "x" + cols + ")");
            /* and the FORCED optimum itself, through every form */
            for (let j = 0; j < cols; j++)
                assert(Math.abs(m.coef[j] - w[j]) < 1e-6,
                       "1 " + tag + " recovers w[" + j + "] (" + rows + "x" + cols + " seed " + seed +
                       ") got " + m.coef[j]);
            m.close(); S.close();
        }
        md.close();
    }
}

/* ==================================================================== *
 *  2. bit-equal under inexact products and hostile magnitudes
 *
 *  Small-integer systems cannot expose a rounding mismatch -- their products
 *  are exact. These LCG matrices span 1e-12..1e12 with structural zeros, so
 *  almost every product rounds and any difference in accumulation rounding
 *  (an FMA contraction, a different addend order) shows up as a moved bit.
 *  The sparse fit must still be bit-equal to the dense fit -- this row is
 *  what a mutation that re-fuses the pair products dies on.
 * ==================================================================== */
function data(rows, cols, seed, nozero) {
    let s = seed >>> 0;
    const rnd = () => { s = (s * 1664525 + 1013904223) >>> 0; return s / 4294967296; };
    const X = [];
    for (let i = 0; i < rows; i++) {
        const r = [];
        for (let j = 0; j < cols; j++)
            r.push((!nozero && rnd() < 0.5) ? 0 : (rnd() - 0.5) * Math.pow(10, Math.floor(rnd() * 24) - 12));
        X.push(r);
    }
    const y = X.map(r => r.reduce((a, v, j) => a + v * (j % 2 ? 1 : -1), 0.5));
    return { X, y };
}
function gridN(cols, seed) {
    const rnd = lcg(seed), rows = [];
    for (const base of [[1, 2, 3], [0, 0, 0], [-1e5, 1e-5, 7], [3.5, -2.25, 0.125]]) {
        const r = [];
        for (let j = 0; j < cols; j++) r.push(base[j % base.length] * (j >= 3 ? 3 : 1));
        rows.push(r);
    }
    rows.push(new Array(cols).fill(0).map(() => Math.round((rnd() * 4 - 2) * 2) / 2));
    return rows;
}
for (const seed of [1, 2, 3, 4, 5]) {
    for (const [rows, cols, nozero] of [[12, 5, false], [7, 3, true], [25, 8, false], [9, 4, true]]) {
        const { X, y } = data(rows, cols, seed * 31 + cols, nozero);
        const grid = gridN(cols, seed);
        const md = new LinearRegression().fit(X, y);
        const pd = md.predict(grid);
        for (const [tag, S] of forms(X)) {
            const m = new LinearRegression().fit(S, y);
            assert(same(m.coef, md.coef) && Object.is(m.intercept, md.intercept),
                   "2 " + tag + " bit-equal coefs (" + rows + "x" + cols + " seed " + seed +
                   " nozero " + nozero + ") " + JSON.stringify(m.coef) + " vs " + JSON.stringify(md.coef));
            assert(same(m.predict(grid), pd),
                   "2 " + tag + " bit-equal predictions (" + rows + "x" + cols + " seed " + seed + ")");
            m.close(); S.close();
        }
        /* the weighted arm gets the same treatment: one computation, weights
         * multiplied in at the same place on both arms */
        const rndw = lcg(seed * 17 + rows);
        const sw = y.map(() => 0.25 + rndw() * 3);
        const mdw = new LinearRegression().fit(X, y, { sampleWeight: sw });
        const pdw = mdw.predict(grid);
        for (const [tag, S] of forms(X)) {
            const m = new LinearRegression().fit(S, y, { sampleWeight: sw });
            assert(same(m.coef, mdw.coef) && Object.is(m.intercept, mdw.intercept) &&
                   same(m.predict(grid), pdw),
                   "2w " + tag + " weighted bit-equal (" + rows + "x" + cols + " seed " + seed + ")");
            m.close(); S.close();
        }
        md.close(); mdw.close();
    }
}

/* ==================================================================== *
 *  3. THE INVARIANT: fit(S) is bit-equal to fit(S.toDense()) for ANY
 *  constructor-legal S -- including shapes that no dense matrix round-trips
 *  to: scattered duplicates, zero-summing duplicate groups, -0 entries,
 *  wildly unsorted rows.
 * ==================================================================== */
{
    const hostile = [
        /* scattered duplicates through an unsorted row */
        new CSR([2, 5, 3, 1, 1, 4], [0, 2, 2, 1, 1, 1], [0, 6], 3),
        /* fully reversed rows with 3-way duplicates */
        (() => {
            const val = [], col = [], ptr = [0];
            for (const r of [[3, 0, 1.5, 2], [0, 4, 0, 8], [1, 1, 1, 1]]) {
                for (let c = 3; c >= 0; c--)
                    if (r[c] !== 0)
                        for (const p of dupSplit(r[c], 3)) { val.push(p); col.push(c); }
                ptr.push(val.length);
            }
            return new CSR(val, col, ptr, 4);
        })(),
        /* zero-summing duplicate groups beside real values: the group reads
         * as an implicit zero and must fit exactly as one */
        new CSR([7, 2, -2, 1e16, 1, -1, 0.5], [0, 1, 1, 2, 2, 2, 0], [0, 3, 7], 3),
        /* -0.0 and exact zeros stored as values */
        new CSR([-0, 5, 0, -3], [0, 1, 1, 2], [0, 4], 3),
        /* empty rows and an all-zero row between nonempty ones */
        new CSR([1, 2], [2, 0], [0, 1, 1, 2, 2], 3),
    ];
    const y = [1.25, -2.5, 0.75, 3, 8];
    const gridFor = (cols) => [[1, 2, 3, 4], [0, 0, 0, 0], [-3, 0.5, 7, -1], [1e3, -1e-3, 2, 0.25]]
        .map(r => r.slice(0, cols));
    for (let k = 0; k < hostile.length; k++) {
        const S = hostile[k];
        const D = S.toDense();
        const grid = gridFor(S.cols);
        const yy = y.slice(0, S.rows);
        const md = new LinearRegression().fit(D, yy);
        const ms = new LinearRegression().fit(S, yy);
        assert(same(ms.coef, md.coef) && Object.is(ms.intercept, md.intercept),
               "3 hostile[" + k + "] fit(S) == fit(S.toDense()) bitwise " +
               JSON.stringify(ms.coef) + " vs " + JSON.stringify(md.coef));
        assert(same(ms.predict(grid), md.predict(grid)),
               "3 hostile[" + k + "] predictions bitwise");
        /* predict on the raw S is the same reading everywhere */
        const ps = ms.predict(S), pd = ms.predict(D);
        assert(same(ps, pd), "3 hostile[" + k + "] predict(S) == predict(toDense) bitwise");
        md.close(); ms.close(); S.close();
    }
}

/* ==================================================================== *
 *  4. the finite-policy parity edge (both directions, all families)
 *
 *  [1e308, 1e308] at one column: every STORED value is finite, the summed
 *  READING is Infinity. Whichever policy a family has for a dense Infinity
 *  row, it must have for this row -- refusing families refuse BOTH forms,
 *  accepting families accept BOTH forms with bit-identical answers.
 * ==================================================================== */
{
    const Xt = [[1, 0, 2], [0, 3, 0], [4, 0, 5], [1, 1, 1], [2, 2, 0], [0, 1, 3]];
    const yt = [1, 2, 3, 2, 2, 3], yr = [1.5, 2.5, 3.5, 2.0, 2.2, 3.1];
    const fam = [];
    fam.push(["LinearRegression", new LinearRegression().fit(Xt, yr), (m, X) => m.predict(X), "accept"]);
    fam.push(["LinearRegression.predictInto", new LinearRegression().fit(Xt, yr),
              (m, X) => m.predictInto(new Float64Array(2), X), "accept"]);
    {
        const m = new LogisticRegression({ maxIter: 400 }).fit(Xt, yt);
        fam.push(["LogisticRegression", m, (mm, X) => mm.predict(X), "accept"]);
        fam.push(["LogisticRegression.predictProba", m, (mm, X) => mm.predictProba(X), "accept"]);
    }
    fam.push(["DecisionTreeClassifier", new DecisionTreeClassifier({ seed: 1 }).fit(Xt, yt), (m, X) => m.predict(X), "accept"]);
    fam.push(["DecisionTreeRegressor", new DecisionTreeRegressor({ seed: 1 }).fit(Xt, yr), (m, X) => m.predict(X), "accept"]);
    {
        const m = new RandomForestClassifier({ nEstimators: 3, seed: 2 }).fit(Xt, yt);
        fam.push(["RandomForestClassifier", m, (mm, X) => mm.predict(X), "accept"]);
        fam.push(["RandomForestClassifier.predictProba", m, (mm, X) => mm.predictProba(X), "accept"]);
    }
    fam.push(["RandomForestRegressor", new RandomForestRegressor({ nEstimators: 3, seed: 2 }).fit(Xt, yr), (m, X) => m.predict(X), "accept"]);
    fam.push(["GradientBoostingClassifier", new GradientBoostingClassifier({ nEstimators: 3, seed: 3 }).fit(Xt, yt), (m, X) => m.predict(X), "accept"]);
    fam.push(["GradientBoostingRegressor", new GradientBoostingRegressor({ nEstimators: 3, seed: 3 }).fit(Xt, yr), (m, X) => m.predict(X), "accept"]);
    fam.push(["XGBClassifier", new XGBClassifier({ nEstimators: 3, seed: 4 }).fit(Xt, yt), (m, X) => m.predict(X), "accept"]);
    fam.push(["XGBRegressor", new XGBRegressor({ nEstimators: 3, seed: 4 }).fit(Xt, yr), (m, X) => m.predict(X), "accept"]);
    fam.push(["KMeans", new KMeans(3, 7).fit(Xt), (m, X) => m.predict(X), "refuse"]);
    fam.push(["GaussianNB", new GaussianNB().fit(Xt, yt), (m, X) => m.predict(X), "refuse"]);
    fam.push(["GaussianNB.predictProba", new GaussianNB().fit(Xt, yt), (m, X) => m.predictProba(X), "refuse"]);
    fam.push(["KNClassifier", new KNClassifier(3).fit(Xt, yt), (m, X) => m.predict(X), "refuse"]);
    fam.push(["KNRegressor", new KNRegressor(3).fit(Xt, yr), (m, X) => m.predict(X), "refuse"]);
    {
        const m = new SVC().fit(Xt, yt);
        fam.push(["SVC", m, (mm, X) => mm.predict(X), "refuse"]);
        fam.push(["SVC.decisionFunction", m, (mm, X) => mm.decisionFunction(X), "refuse"]);
    }
    {
        const m = new GaussianMixture(2, { seed: 6 }).fit(Xt);
        fam.push(["GaussianMixture", m, (mm, X) => mm.predict(X), "refuse"]);
        fam.push(["GaussianMixture.predictProba", m, (mm, X) => mm.predictProba(X), "refuse"]);
    }
    const outcome = (fn) => {
        try { return "ACCEPTED:" + JSON.stringify(flat(fn())); }
        catch (e) { return "refused:" + e.constructor.name; }
    };
    /* overflow-class rows: both signs, and an intermediate overflow whose
     * total would be finite in exact arithmetic ([1.5e308, 1.5e308, -1.7e308]
     * sums to 1.3e308 exactly but reads Infinity -- the reading is the
     * storage-order sum and every consumer must agree on it) */
    for (const pair of [[1e308, 1e308], [-1e308, -1e308], [1.5e308, 1.5e308, -1.7e308], [1e308, 1e308, -1e308]]) {
        const S = new CSR(pair, pair.map(() => 0), [0, pair.length], 3);
        const D = S.toDense();
        assert(!isFinite(D[0][0]), "4 setup: " + pair + " reads " + D[0][0]);
        for (const [name, m, fn, want] of fam) {
            const od = outcome(() => fn(m, D)), os = outcome(() => fn(m, S));
            assert(od === os,
                   "4 " + name + " parity on overflow dup " + pair + ": dense=" + od + " csr=" + os);
            assert((want === "refuse") === od.startsWith("refused"),
                   "4 " + name + " policy (" + want + ") holds at the overflow reading: " + od);
            if (od.startsWith("refused"))
                assert(od.includes("RangeError"),
                       "4 " + name + " refuses the overflow reading with RangeError: " + od);
        }
        /* fit-side parity: both linear fits refuse BOTH forms */
        const fits = [
            ["LinearRegression.fit", (X) => new LinearRegression().fit(X, [1])],
            ["LogisticRegression.fit", (X) => new LogisticRegression({ maxIter: 10 }).fit(X, [1])],
        ];
        for (const [name, fn] of fits) {
            const od = outcome(() => fn(D)), os = outcome(() => fn(S));
            assert(od === os && od.startsWith("refused") && od.includes("RangeError"),
                   "4 " + name + " refuses the overflow reading identically: " + od + " / " + os);
        }
        S.close();
    }
    const seen = new Set();
    for (const [, m] of fam) if (!seen.has(m) && typeof m.close === "function") { seen.add(m); try { m.close(); } catch (e) {} }
}

/* ==================================================================== *
 *  5. constructor symmetry: new CSR(...) and CSR.fromDense(...) draw
 *  the line at the same place -- a CSR never carries a non-finite value.
 * ==================================================================== */
{
    for (const bad of [NaN, Infinity, -Infinity]) {
        const ctorMsg = throws(() => new CSR([bad, 1], [0, 1], [0, 2], 3), RangeError,
                               "5 ctor refuses " + String(bad));
        assert(/value finite/.test(ctorMsg), "5 ctor message names the rule: " + ctorMsg);
        const fdMsg = throws(() => CSR.fromDense([[bad, 1]]), RangeError,
                             "5 fromDense refuses " + String(bad));
        assert(/keeps finite values only/.test(fdMsg) &&
               (isNaN(bad) ? /is NaN/.test(fdMsg) : /is infinite/.test(fdMsg)),
               "5 fromDense message names the class and the cell: " + fdMsg);
        /* and the cell is named at any position */
        const m2 = throws(() => CSR.fromDense([[1, 2], [3, bad]]), RangeError,
                          "5 fromDense names the exact cell");
        assert(/\[1\]\[1\]/.test(m2), "5 the cell is X[1][1]: " + m2);
    }
    /* finite values still construct both ways, and both ways agree */
    const A = new CSR([1, 2], [0, 1], [0, 2], 3);
    const B = CSR.fromDense([[1, 2, 0]]);
    assert(same(A.toDense(), B.toDense()), "5 finite construction agrees");
    A.close(); B.close();
    /* the flat form of fromDense refuses the same way */
    throws(() => CSR.fromDense(new Float64Array([1, NaN, 0]), 1, 3), RangeError,
           "5 fromDense(flat) refuses a NaN");
}

/* ==================================================================== *
 *  6. the expansion guard boundary
 *
 *  The guard is a measured byte budget on the JS-Array materialisation:
 *      24 bytes/cell + 280 bytes/row <= 2^30 (see dyn_ml_dense_fits).
 *  The boundary is a test row in BOTH directions: the admitted maximum
 *  completes with the full width, one past it refuses with the documented
 *  RangeError, and the widths that used to SIGSEGV inside the Array build
 *  (2^31, 2^32-1) now refuse cleanly. If the formula constants move, these
 *  rows move with them -- that is the pin.
 * ==================================================================== */
{
    const budget = Math.pow(2, 30), perCell = 24, perRow = 280;
    const maxCols = Math.floor((budget - perRow) / perCell);        /* 44739231 */
    const maxRowsAt = (c) => Math.floor(budget / (perCell * c + perRow));

    /* one ROW array at the admitted maximum completes with the full length */
    {
        const H = new CSR([1.0], [0], [0, 1], maxCols);
        const r = H.row(0);
        assert(r.length === maxCols, "6 row() at maxCols completes: " + r.length);
        H.close();
    }
    /* one PAST it refuses, with the documented message, in both directions */
    {
        const H = new CSR([1.0], [0], [0, 1], maxCols + 1);
        const e1 = throws(() => H.row(0), RangeError, "6 row() at maxCols+1 refuses");
        assert(/fit in memory/.test(e1), "6 row() names the memory bound: " + e1);
        const e2 = throws(() => H.toDense(), RangeError, "6 toDense() at maxCols+1 refuses");
        assert(/fit in memory/.test(e2), "6 toDense() names the memory bound: " + e2);
        H.close();
    }
    /* the row-count dimension of the same formula: admitted max completes,
     * one past refuses */
    {
        const mk = (rows) => {
            const val = new Array(rows).fill(1.0), col = new Array(rows).fill(0);
            const ptr = [];
            for (let i = 0; i <= rows; i++) ptr.push(i);
            return new CSR(val, col, ptr, 1);
        };
        const H = mk(maxRowsAt(1));
        const D = H.toDense();
        assert(D.length === maxRowsAt(1) && D[0].length === 1,
               "6 toDense at maxRows completes: " + D.length + "x" + D[0].length);
        H.close();
        const H2 = mk(maxRowsAt(1) + 1);
        const e = throws(() => H2.toDense(), RangeError, "6 toDense at maxRows+1 refuses");
        assert(/fit in memory/.test(e), "6 names the memory bound: " + e);
        H2.close();
    }
    /* the c-dependence of the formula refuses exactly */
    {
        const rows = maxRowsAt(3) + 1;
        const val = new Array(rows).fill(1.0), col = new Array(rows).fill(0);
        const ptr = [];
        for (let i = 0; i <= rows; i++) ptr.push(i);
        const H = new CSR(val, col, ptr, 3);
        throws(() => H.toDense(), RangeError,
               "6 toDense at maxRows(3)+1 x 3 refuses (" + rows + "x3)");
        H.close();
    }
    /* the old killers: admitted by the old <= 2^32-1 bound, died inside the
     * Array build. Both must refuse cleanly now. */
    for (const cols of [2147483648, 4294967295]) {
        const H = new CSR([1.0], [0], [0, 1], cols);
        const e1 = throws(() => H.row(0), RangeError, "6 row() at " + cols + " refuses");
        assert(/fit in memory/.test(e1), "6 names the memory bound: " + e1);
        throws(() => H.toDense(), RangeError, "6 toDense() at " + cols + " refuses");
        H.close();
    }
}

print("test_ml_fit_parity: all " + n + " assertions passed");
