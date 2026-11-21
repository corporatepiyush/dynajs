// covers: dyna:ml.CSR
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_csr_fit_parity");
import { CSR, LinearRegression, LogisticRegression, KMeans, GaussianNB,
         DecisionTreeClassifier, DecisionTreeRegressor, RandomForestClassifier,
         RandomForestRegressor, GradientBoostingClassifier, GradientBoostingRegressor,
         XGBClassifier, XGBRegressor, KNClassifier, KNRegressor, SVC,
         GaussianMixture } from "dyna:ml";

let n = 0;
function assert(c, m) { n++; if (!c) throw new Error("assertion failed: " + m); }
async function throws(fn, kind, m) {
    n++;
    try { await fn(); } catch (e) {
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

async function forms(X) {
    const out = [];
    out.push(["fromDense", await CSR.fromDense(X)]);
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

await __kitS.section("3 hostile", async () => {
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
        const D = await S.toDense();
        const grid = gridFor(S.cols);
        const yy = y.slice(0, S.rows);
        const md = await new LinearRegression().fit(D, yy);
        const ms = await new LinearRegression().fit(S, yy);
        assert(same(ms.coef, md.coef) && Object.is(ms.intercept, md.intercept),
               "3 hostile[" + k + "] fit(S) == fit(S.toDense()) bitwise " +
               JSON.stringify(ms.coef) + " vs " + JSON.stringify(md.coef));
        assert(same(await ms.predict(grid), await md.predict(grid)),
               "3 hostile[" + k + "] predictions bitwise");
        const ps = await ms.predict(S), pd = await ms.predict(D);
        assert(same(ps, pd), "3 hostile[" + k + "] predict(S) == predict(toDense) bitwise");
        md.close(); ms.close(); S.close();
    }
});

await __kitS.section("5 ctor refuses", async () => {
    for (const bad of [NaN, Infinity, -Infinity]) {
        const ctorMsg = await throws(() => new CSR([bad, 1], [0, 1], [0, 2], 3), RangeError,
                               "5 ctor refuses " + String(bad));
        assert(/value finite/.test(ctorMsg), "5 ctor message names the rule: " + ctorMsg);
        const fdMsg = await throws(async () => await CSR.fromDense([[bad, 1]]), RangeError,
                             "5 fromDense refuses " + String(bad));
        assert(/keeps finite values only/.test(fdMsg) &&
               (isNaN(bad) ? /is NaN/.test(fdMsg) : /is infinite/.test(fdMsg)),
               "5 fromDense message names the class and the cell: " + fdMsg);
        const m2 = await throws(async () => await CSR.fromDense([[1, 2], [3, bad]]), RangeError,
                          "5 fromDense names the exact cell");
        assert(/\[1\]\[1\]/.test(m2), "5 the cell is X[1][1]: " + m2);
    }
    const A = new CSR([1, 2], [0, 1], [0, 2], 3);
    const B = await CSR.fromDense([[1, 2, 0]]);
    assert(same(await A.toDense(), await B.toDense()), "5 finite construction agrees");
    A.close(); B.close();
    await throws(async () => await CSR.fromDense(new Float64Array([1, NaN, 0]), 1, 3), RangeError,
           "5 fromDense(flat) refuses a NaN");
});

await __kitS.section("6 row() at maxCols completes", async () => {
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
        const e1 = await throws(() => H.row(0), RangeError, "6 row() at maxCols+1 refuses");
        assert(/fit in memory/.test(e1), "6 row() names the memory bound: " + e1);
        const e2 = await throws(async () => await H.toDense(), RangeError, "6 toDense() at maxCols+1 refuses");
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
        const D = await H.toDense();
        assert(D.length === maxRowsAt(1) && D[0].length === 1,
               "6 toDense at maxRows completes: " + D.length + "x" + D[0].length);
        H.close();
        const H2 = mk(maxRowsAt(1) + 1);
        const e = await throws(async () => await H2.toDense(), RangeError, "6 toDense at maxRows+1 refuses");
        assert(/fit in memory/.test(e), "6 names the memory bound: " + e);
        H2.close();
    }
    {
        const rows = maxRowsAt(3) + 1;
        const val = new Array(rows).fill(1.0), col = new Array(rows).fill(0);
        const ptr = [];
        for (let i = 0; i <= rows; i++) ptr.push(i);
        const H = new CSR(val, col, ptr, 3);
        await throws(async () => await H.toDense(), RangeError,
               "6 toDense at maxRows(3)+1 x 3 refuses (" + rows + "x3)");
        H.close();
    }
    for (const cols of [2147483648, 4294967295]) {
        const H = new CSR([1.0], [0], [0, 1], cols);
        const e1 = await throws(() => H.row(0), RangeError, "6 row() at " + cols + " refuses");
        assert(/fit in memory/.test(e1), "6 names the memory bound: " + e1);
        await throws(async () => await H.toDense(), RangeError, "6 toDense() at " + cols + " refuses");
        H.close();
    }
});
__kitS.end();

print("test_ml_fit_parity: all " + n + " assertions passed");
