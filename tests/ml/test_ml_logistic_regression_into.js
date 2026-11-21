// covers: dyna:ml.LogisticRegression
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_logistic_regression_into");
import * as ml from "dyna:ml";

let n = 0;
function assert(c, m) { n++; if (!c) throw new Error("assert: " + m); }
async function throws(fn, m) { let t = false; try { await fn(); } catch { t = true; } assert(t, m); }
function eqArr(a, b) { if (a.length !== b.length) return false;
    for (let i = 0; i < a.length; i++) if (!Object.is(a[i], b[i])) return false; return true; }
function mulberry32(seed) { let a = seed >>> 0; return function () {
    a |= 0; a = (a + 0x6D2B79F5) | 0; let t = Math.imul(a ^ (a >>> 15), 1 | a);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t; return ((t ^ (t >>> 14)) >>> 0) / 4294967296; }; }

await __kitS.section("block L14", async () => {
    const rnd = mulberry32(0xC57A);
    const lg = new ml.LogisticRegression({ maxIter: 300 });
    const lin = new ml.LinearRegression();
    const N = 24, F = 6;
    const X = [], yc = [], yr = [];
    for (let i = 0; i < N; i++) {
        const row = [];
        for (let j = 0; j < F; j++) row.push(rnd() * 4 - 2);
        X.push(row);
        yc.push(row[0] + row[1] > 0 ? 1 : 0);
        yr.push(row[0] * 2 - row[2] + 0.5);
    }
    await lg.fit(X, yc);
    await lin.fit(X, yr);
    for (let sweep = 0; sweep < 30; sweep++) {
        const sparse = X.map(row => row.map(v => (rnd() < 0.6 ? 0 : v)));
        const csr = await ml.CSR.fromDense(sparse);
        assert(eqArr([...await lin.predict(csr)], [...await lin.predict(sparse)]),
            `sweep ${sweep}: linreg CSR == dense`);
        assert(eqArr([...await lg.predict(csr)], [...await lg.predict(sparse)]),
            `sweep ${sweep}: logreg CSR == dense`);
        const pd = await lg.predictProba(sparse), ps = await lg.predictProba(csr);
        for (let i = 0; i < N; i++)
            assert(eqArr(pd[i], ps[i]), `sweep ${sweep}: proba row ${i}`);
        const out = new Float64Array(N);
        await lin.predictInto(out, csr);
        assert(eqArr([...out], [...await lin.predict(sparse)]),
            `sweep ${sweep}: predictInto(CSR) == dense`);
    }
});
__kitS.end();

console.log(`test_ml_into: ${n} assertions ok`);
