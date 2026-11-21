// covers: dyna:ml.CSR
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_csr_linreg_exact");
import { LinearRegression, CSR } from "dyna:ml";
import { SYSTEMS, BAND, NEAR } from "../lib/ml_linreg_table.js";

let n = 0;
function assert(c, msg) { n++; if (!c) throw new Error("assertion failed: " + msg); }
function relerr(got, want) {
    return Math.abs(got - want) / Math.max(1, Math.abs(want));
}

await __kitS.section("coefficient", async () => {
for (const b of BAND) {
    const opt = b.w ? { sampleWeight: b.w } : undefined;
    const m = await new LinearRegression().fit(b.X, b.y, opt);
    const coef = m.coef.concat([m.intercept]);
    for (let j = 0; j < coef.length; j++)
        assert(Number.isFinite(coef[j]),
            b.name + " (" + b.note + "): coefficient " + j + " is finite, got " + coef[j]);
    const oraMax = Math.max(1, ...b.oracleCoef.map(Math.abs));
    for (let j = 0; j < coef.length; j++)
        assert(Math.abs(coef[j]) <= 8 * oraMax,
            b.name + ": no 1/residue amplification -- |coef[" + j + "]| = " +
            Math.abs(coef[j]).toExponential(3) + " <= 8 x " + oraMax);
    const gotPred = (await m.predict([b.probe]))[0];
    assert(Number.isFinite(gotPred) &&
           Math.abs(gotPred - b.oraclePred) <= 1e-4 * Math.max(1, Math.abs(b.oraclePred)),
        b.name + ": the identifiable prediction is the oracle's, got " + gotPred +
        " want " + b.oraclePred);
    if (b.realizable) {
        const tp = await m.predict(b.X);
        for (let i = 0; i < tp.length; i++)
            assert(Math.abs(tp[i] - b.y[i]) <= 1e-4 * Math.max(1, Math.abs(b.y[i])),
                b.name + ": training prediction " + i + " recovers the target, got " +
                tp[i] + " want " + b.y[i]);
    }
    const s = await new LinearRegression().fit(await CSR.fromDense(b.X), b.y, opt);
    assert(s.coef.length === m.coef.length &&
           s.coef.every((v, i) => Object.is(v, m.coef[i])) &&
           Object.is(s.intercept, m.intercept),
        b.name + ": a CSR fit is bit-identical to the dense fit");
    assert(Object.is((await s.predict([b.probe]))[0], gotPred),
        b.name + ": the CSR prediction is bit-identical");
    m.close(); s.close();
}
});
__kitS.end();


print("test_ml_linreg_exact: all " + n + " assertions passed");
