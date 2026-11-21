// covers: dyna:ml.LinearRegression
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_linear_regression_linreg_exact");
import { LinearRegression, CSR } from "dyna:ml";
import { SYSTEMS, BAND, NEAR } from "../lib/ml_linreg_table.js";

let n = 0;
function assert(c, msg) { n++; if (!c) throw new Error("assertion failed: " + msg); }
function relerr(got, want) {
    return Math.abs(got - want) / Math.max(1, Math.abs(want));
}

for (const s of SYSTEMS) {
    const m = await new LinearRegression().fit(s.X, s.y);
    const got = m.coef.concat([m.intercept]);
    let worst = 0, worstOracle = 0;
    for (let j = 0; j < got.length; j++) {
        worst = Math.max(worst, relerr(got[j], s.want[j]));
        worstOracle = Math.max(worstOracle, relerr(got[j], s.oracle[j]));
    }
    assert(worst <= 1e-9,
        s.name + " (" + s.note + "): exact OLS to <= 1e-9 relative, got " +
        worst.toExponential(3) + " coef=" + JSON.stringify(got) +
        " want=" + JSON.stringify(s.want));
    assert(worstOracle <= 1e-9,
        s.name + ": within 1e-9 relative of the numpy lstsq oracle, got " +
        worstOracle.toExponential(3));
    const pred = await m.predict(s.X);
    for (let i = 0; i < pred.length; i++)
        assert(Math.abs(pred[i] - s.y[i]) <= 1e-6 * Math.max(1, Math.abs(s.y[i])),
            s.name + ": prediction " + i + " recovers the target");
}

async function singularRow(X, y, probe, wantPred, label) {
    const m = await new LinearRegression().fit(X, y);
    for (const c of m.coef.concat([m.intercept]))
        assert(Number.isFinite(c), label + ": coefficients stay finite");
    const p = (await m.predict([probe]))[0];
    assert(Math.abs(p - wantPred) <= 1e-5 * Math.max(1, Math.abs(wantPred)),
        label + ": the identifiable prediction is recovered, got " + p +
        " want " + wantPred);
}

await singularRow([[1, 1], [2, 2], [3, 3]], [1, 2, 3], [4, 4], 4,
    "duplicate columns");
await __kitS.section("a zero column is fitted as exactly 0", async () => {
    const m = await new LinearRegression().fit([[1, 0], [2, 0], [3, 0]], [2, 4, 6]);
    assert(m.coef[1] === 0, "a zero column is fitted as exactly 0");
    assert(Math.abs(m.coef[0] - 2) <= 1e-5, "the live column is recovered");
});

await singularRow([[1, 2], [2, 4], [3, 6]], [3, 6, 9], [4, 8], 12,
    "integer collinear columns");
await singularRow([[1, 5], [1, 5], [1, 5]], [2, 2, 2], [1, 5], 2,
    "constant columns");
await __kitS.section("an empty fit refuses with TypeError", async () => {
    let threw = false;
    try { await new LinearRegression().fit([], []); } catch (e) {
        threw = e instanceof TypeError;
    }
    assert(threw, "an empty fit refuses with TypeError");
});

await __kitS.section("a CSR fit is bit-identical to the dense fit of the same reading", async () => {
    const X = [[1, 0, 2], [0, 3, 0], [4, 0, 5], [1, 1, 0]];
    const y = [7, -2, 11, 4];
    const a = await new LinearRegression().fit(X, y);
    const b = await new LinearRegression().fit(await CSR.fromDense(X), y);
    assert(a.coef.length === b.coef.length &&
           a.coef.every((v, i) => Object.is(v, b.coef[i])) &&
           Object.is(a.intercept, b.intercept),
        "a CSR fit is bit-identical to the dense fit of the same reading");
});

await __kitS.section("all-ones weights are BIT-identical to no weights", async () => {
    const X = [[1], [2], [3], [4]], y = [1, 2, 3, 10];
    const plain = await new LinearRegression().fit(X, y);
    const ones = await new LinearRegression().fit(X, y, { sampleWeight: [1, 1, 1, 1] });
    assert(plain.coef.every((v, i) => Object.is(v, ones.coef[i])) &&
           Object.is(plain.intercept, ones.intercept),
        "all-ones weights are BIT-identical to no weights");
    const zeroed = await new LinearRegression().fit(X, y, { sampleWeight: [1, 1, 1, 0] });
    const deleted = await new LinearRegression().fit([[1], [2], [3]], [1, 2, 3]);
    assert(Math.abs((await zeroed.predict([[10]]))[0] - (await deleted.predict([[10]]))[0]) <= 1e-7,
        "weight 0 == the row not being there (to ridge/rounding)");
});

await __kitS.section("the unit-weight reference prediction", async () => {
    const X = [[1, 1], [2, 2], [3, 3]], y = [1, 2, 3], probe = [4, 4];
    const unit = await new LinearRegression().fit(X, y);
    const unitPred = (await unit.predict([probe]))[0];
    assert(Math.abs(unitPred - 4) <= 1e-4, "the unit-weight reference prediction");
    const pow2 = await new LinearRegression().fit(X, y, { sampleWeight: [Math.pow(2, 1000), Math.pow(2, 1000), Math.pow(2, 1000)] });
    assert(Number.isFinite(pow2.coef[0]) && Number.isFinite(pow2.intercept),
        "weights 2^1000: finite coefficients");
    assert(pow2.coef.every((v, i) => Object.is(v, unit.coef[i])) &&
           Object.is(pow2.intercept, unit.intercept),
        "weights 2^1000 are bit-identical to unit weights (exact power-of-two scale)");
    const huge = await new LinearRegression().fit(X, y, { sampleWeight: [1e300, 1e300, 1e300] });
    const hugeCoef = huge.coef.concat([huge.intercept]);
    assert(hugeCoef.every(Number.isFinite), "weights 1e300: finite coefficients, got " + JSON.stringify(hugeCoef));
    // The solution is ~[0.5, 0.5, 0]; whether the largest lands a few ulp above or
    // below 0.5 depends on FMA fusion (arm64 fuses, baseline x86-64 cannot).
    assert(Math.max(...hugeCoef.map(Math.abs)) > 0.25,
        "weights 1e300: NOT the silently-zero model, got " + JSON.stringify(hugeCoef));
    assert(Math.abs((await huge.predict([probe]))[0] - unitPred) <= 1e-6 * Math.max(1, Math.abs(unitPred)),
        "weights 1e300: the identifiable prediction matches the unit-weight fit, got " +
        (await huge.predict([probe]))[0]);
    const tiny = await new LinearRegression().fit(X, y, { sampleWeight: [1e-320, 1e-320, 1e-320] });
    assert(tiny.coef.concat([tiny.intercept]).every(Number.isFinite),
        "subnormal weights: finite coefficients");
    assert(Math.abs((await tiny.predict([probe]))[0] - unitPred) <= 1e-6 * Math.max(1, Math.abs(unitPred)),
        "subnormal weights: the identifiable prediction matches the unit-weight fit");
    const zx = await new LinearRegression().fit([[0], [0]], [1, 2], { sampleWeight: [1e-320, 1e-320] });
    assert(zx.coef.concat([zx.intercept]).every(Number.isFinite),
        "subnormal weights + zero column: finite, got " + JSON.stringify(zx.coef));
    assert(Math.abs((await zx.predict([[0]]))[0] - 1.5) <= 1e-6,
        "subnormal weights + zero column: the least-squares intercept is the mean, got " +
        (await zx.predict([[0]]))[0]);
    const mixed = await new LinearRegression().fit(X, y, { sampleWeight: [1e-320, 1, 1] });
    assert(mixed.coef.concat([mixed.intercept]).every(Number.isFinite),
        "mixed subnormal weights: finite coefficients");
    unit.close(); pow2.close(); huge.close(); tiny.close(); zx.close(); mixed.close();
    let overflowThrew = null;
    try {
        await new LinearRegression().fit([[1, 1e6], [2, 1e6], [3, 1e6], [4, 1e6]],
            [3, 5, 7, 9], { sampleWeight: [1e300, 1e300, 1e300, 1e300] });
    } catch (e) {
        overflowThrew = e;
    }
    assert(overflowThrew !== null &&
           /singular system/.test(overflowThrew.message),
        "1e300 weights on 1e6-scale features: the documented overflow limit " +
        "refuses with 'singular system', got " + overflowThrew);
});
__kitS.end();

for (const nr of NEAR) {
    const bound = nr.side === "exact" ? 1e-12 : 1e-6;
    const m = await new LinearRegression().fit(nr.X, nr.y);
    const coef = m.coef.concat([m.intercept]);
    for (let j = 0; j < coef.length; j++)
        assert(Number.isFinite(coef[j]),
            nr.name + " (" + nr.note + "): coefficient " + j + " is finite, got " + coef[j]);
    if (!nr.singular) {
        let worst = 0, worstOracle = 0;
        for (let j = 0; j < coef.length; j++) {
            worst = Math.max(worst, relerr(coef[j], nr.want[j]));
            worstOracle = Math.max(worstOracle, relerr(coef[j], nr.oracle[j]));
        }
        assert(worst <= 1e-6,
            nr.name + ": coefficients within 1e-6 relative of the exact " +
            "OLS answer, got " + worst.toExponential(3) +
            " coef=" + JSON.stringify(coef) + " want=" + JSON.stringify(nr.want));
        assert(worstOracle <= 1e-6,
            nr.name + ": coefficients within 1e-6 relative of the numpy " +
            "lstsq oracle, got " + worstOracle.toExponential(3));
    } else {
        const mag = coef.reduce((s, v) => s + Math.abs(v), 0);
        assert(mag > 0.5,
            nr.name + ": NOT the silently-zero model, coef=" + JSON.stringify(coef));
    }
    const tp = await m.predict(nr.X);
    for (let i = 0; i < tp.length; i++)
        assert(relerr(tp[i], nr.y[i]) <= bound,
            nr.name + ": training prediction " + i + " within " + bound +
            " relative, got " + tp[i] + " want " + nr.y[i]);
    const pp = (await m.predict([nr.probe]))[0];
    assert(relerr(pp, nr.probeWant) <= bound,
        nr.name + ": probe prediction within " + bound + " relative, got " +
        pp + " want " + nr.probeWant);
    m.close();
}


print("test_ml_linreg_exact: all " + n + " assertions passed");
