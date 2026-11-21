// covers: dyna:ml.CSR
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_csr_into");
import * as ml from "dyna:ml";

let n = 0;
function assert(c, m) { n++; if (!c) throw new Error("assert: " + m); }
async function throws(fn, m) { let t = false; try { await fn(); } catch { t = true; } assert(t, m); }
function eqArr(a, b) { if (a.length !== b.length) return false;
    for (let i = 0; i < a.length; i++) if (!Object.is(a[i], b[i])) return false; return true; }
function mulberry32(seed) { let a = seed >>> 0; return function () {
    a |= 0; a = (a + 0x6D2B79F5) | 0; let t = Math.imul(a ^ (a >>> 15), 1 | a);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t; return ((t ^ (t >>> 14)) >>> 0) / 4294967296; }; }

await __kitS.section("logreg CSR predict == dense", async () => {
    const X = [
        [1, 0, 0, 2.5],
        [0, 0, 3, 0],
        [0, 4, 0, 0],
        [2, 0, 0, 1],
    ];
    const yc = [0, 1, 1, 0];
    const yr = [3.5, 3, 4, 3];

    const lg = new ml.LogisticRegression({ maxIter: 500 });
    await lg.fit(X, yc);
    const csr = await ml.CSR.fromDense(X);
    assert(eqArr([...await lg.predict(csr)], [...await lg.predict(X)]), "logreg CSR predict == dense");
    const pd = await lg.predictProba(X), ps = await lg.predictProba(csr);
    assert(pd.length === ps.length, "logreg CSR proba shape");
    for (let i = 0; i < pd.length; i++)
        assert(eqArr(pd[i], ps[i]), `logreg CSR proba row ${i}`);
    {
        const out = new Float64Array(X.length);
        assert((await lg.predictInto(out, csr)) === X.length, "logreg predictInto(CSR)");
        assert(eqArr([...out], [...await lg.predict(X)]), "logreg predictInto(CSR) values");
    }
    const lin = new ml.LinearRegression();
    await lin.fit(X, yr);
    assert(eqArr([...await lin.predict(csr)], [...await lin.predict(X)]), "linreg CSR predict == dense");

    {
        const s = new ml.CSR([1, 1], [3, 0], [0, 2], 4);
        const d = [[1, 0, 0, 1]];
        assert(eqArr([...await lin.predict(s)], [...await lin.predict(d)]),
            "unsorted column order inside a row");
    }
    {
        const s = new ml.CSR([1, 1], [0, 0], [0, 2], 4);
        const d = [[2, 0, 0, 0]];
        assert(eqArr([...await lin.predict(s)], [...await lin.predict(d)]),
            "duplicate column indices sum");
    }
    {
        const rf = new ml.RandomForestRegressor({ nEstimators: 4, seed: 1 });
        await rf.fit(X, yr);
        assert(eqArr([...await rf.predict(csr)], [...await rf.predict(X)]),
            "forest CSR predict == dense");
    }
    {
        const rf = new ml.RandomForestRegressor({ nEstimators: 4, seed: 1 });
        let msg = "";
        try { await rf.fit(csr, yr); } catch (e) { msg = e.message; }
        assert(msg.includes("toDense"), "fit refusal names toDense()");
    }
});
__kitS.end();

console.log(`test_ml_into: ${n} assertions ok`);
