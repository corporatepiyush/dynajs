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

function dupSplit(v, k) {
    const out = [v / Math.pow(2, k - 1), v / Math.pow(2, k - 1)];
    for (let j = k - 2; j >= 1; j--) out.push(v / Math.pow(2, j));
    return out;
}

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

for (const seed of [1, 2, 3]) {
    for (const [rows, cols] of [[4, 2], [5, 3], [12, 5], [8, 3]]) {
        const rnd = lcg(seed * 100 + cols);
        const w = [];
        for (let j = 0; j < cols; j++) w.push(((seed * 7 + j * 13) % 17) - 8 + 0.5 * (j % 2));
        const X = [];
        for (let j = 0; j < cols; j++) {
            const r = new Array(cols).fill(0);
            r[j] = 1;
            X.push(r);
        }
        while (X.length < rows) {
            const r = new Array(cols).fill(0);
            for (let j = 0; j < cols; j++)
                if (rnd() < 0.5) {
                    const v = Math.round((rnd() * 4 - 2) * 2) / 2;
                    r[j] = v === 0 ? 0 : v;
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
            for (let j = 0; j < cols; j++)
                assert(Math.abs(m.coef[j] - w[j]) < 1e-6,
                       "1 " + tag + " recovers w[" + j + "] (" + rows + "x" + cols + " seed " + seed +
                       ") got " + m.coef[j]);
            m.close(); S.close();
        }
        md.close();
    }
}

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

{
    const hostile = [
        new CSR([2, 5, 3, 1, 1, 4], [0, 2, 2, 1, 1, 1], [0, 6], 3),
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
        new CSR([7, 2, -2, 1e16, 1, -1, 0.5], [0, 1, 1, 2, 2, 2, 0], [0, 3, 7], 3),
        new CSR([-0, 5, 0, -3], [0, 1, 1, 2], [0, 4], 3),
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
        const ps = ms.predict(S), pd = ms.predict(D);
        assert(same(ps, pd), "3 hostile[" + k + "] predict(S) == predict(toDense) bitwise");
        md.close(); ms.close(); S.close();
    }
}

{
    const Xt = [[1, 0, 2], [0, 3, 0], [4, 0, 5], [1, 1, 1], [2, 2, 0], [0, 1, 3]];
    const yt = [1, 2, 3, 2, 2, 3], yr = [1.5, 2.5, 3.5, 2.0, 2.2, 3.1];
    const fam = [];
    fam.push(["LinearRegression", new LinearRegression().fit(Xt, yr), (m, X) => m.predict(X), "refuse"]);
    fam.push(["LinearRegression.predictInto", new LinearRegression().fit(Xt, yr),
              (m, X) => m.predictInto(new Float64Array(2), X), "refuse"]);
    {
        const m = new LogisticRegression({ maxIter: 400 }).fit(Xt, yt);
        fam.push(["LogisticRegression", m, (mm, X) => mm.predict(X), "refuse"]);
        fam.push(["LogisticRegression.predictProba", m, (mm, X) => mm.predictProba(X), "refuse"]);
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
        const m2 = throws(() => CSR.fromDense([[1, 2], [3, bad]]), RangeError,
                          "5 fromDense names the exact cell");
        assert(/\[1\]\[1\]/.test(m2), "5 the cell is X[1][1]: " + m2);
    }
    const A = new CSR([1, 2], [0, 1], [0, 2], 3);
    const B = CSR.fromDense([[1, 2, 0]]);
    assert(same(A.toDense(), B.toDense()), "5 finite construction agrees");
    A.close(); B.close();
    throws(() => CSR.fromDense(new Float64Array([1, NaN, 0]), 1, 3), RangeError,
           "5 fromDense(flat) refuses a NaN");
}

{
    const budget = Math.pow(2, 30), perCell = 24, perRow = 280;
    const maxCols = Math.floor((budget - perRow) / perCell);
    const maxRowsAt = (c) => Math.floor(budget / (perCell * c + perRow));

    {
        const H = new CSR([1.0], [0], [0, 1], maxCols);
        const r = H.row(0);
        assert(r.length === maxCols, "6 row() at maxCols completes: " + r.length);
        H.close();
    }
    {
        const H = new CSR([1.0], [0], [0, 1], maxCols + 1);
        const e1 = throws(() => H.row(0), RangeError, "6 row() at maxCols+1 refuses");
        assert(/fit in memory/.test(e1), "6 row() names the memory bound: " + e1);
        const e2 = throws(() => H.toDense(), RangeError, "6 toDense() at maxCols+1 refuses");
        assert(/fit in memory/.test(e2), "6 toDense() names the memory bound: " + e2);
        H.close();
    }
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
    for (const cols of [2147483648, 4294967295]) {
        const H = new CSR([1.0], [0], [0, 1], cols);
        const e1 = throws(() => H.row(0), RangeError, "6 row() at " + cols + " refuses");
        assert(/fit in memory/.test(e1), "6 names the memory bound: " + e1);
        throws(() => H.toDense(), RangeError, "6 toDense() at " + cols + " refuses");
        H.close();
    }
}

print("test_ml_fit_parity: all " + n + " assertions passed");
