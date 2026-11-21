// covers: dyna:ml.StandardScaler
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_standard_scaler_preprocessing");
import { StandardScaler, MinMaxScaler, meanSquaredError, meanAbsoluteError,
         r2Score, logLoss, accuracy, confusionMatrix } from "dyna:ml";

let n = 0;
function assert(cond, msg) { n++; if (!cond) throw new Error("assertion failed: " + msg); }
function approx(a, b, eps, msg) {
    n++;
    if (!(Math.abs(a - b) <= eps))
        throw new Error(`approx failed: ${msg} (${a} vs ${b})`);
}
function approxArr(a, b, eps, msg) {
    assert(a.length === b.length, msg + ": length");
    for (let i = 0; i < a.length; i++) approx(a[i], b[i], eps, `${msg}[${i}]`);
}
async function throws(fn, msg, ErrType) {
    n++;
    try { await fn(); } catch (e) {
        if (ErrType && !(e instanceof ErrType))
            throw new Error(`${msg}: wrong error type ${e.constructor.name}`);
        return;
    }
    throw new Error("expected a throw: " + msg);
}

await __kitS.section("fit returns this", async () => {
    const X = [[1, 10], [2, 20], [3, 30], [4, 40]];
    const s = new StandardScaler();
    try {
        assert((await s.fit(X)) === s, "fit returns this");
        approxArr(s.mean, [2.5, 25], 1e-12, "mean");
        approxArr(s.std, [Math.sqrt(1.25), Math.sqrt(125)], 1e-12, "population std (ddof=0)");

        const z = await s.transform(X);
        assert(Array.isArray(z) && Array.isArray(z[0]), "Array in -> Array of Arrays out");
        let c0 = 0, c1 = 0;
        for (const row of z) { c0 += row[0]; c1 += row[1]; }
        approx(c0, 0, 1e-12, "column 0 of z sums to 0");
        approx(c1, 0, 1e-12, "column 1 of z sums to 0");
        approx(z[0][0], (1 - 2.5) / Math.sqrt(1.25), 1e-12, "z[0][0]");
        approx(z[3][1], (40 - 25) / Math.sqrt(125), 1e-12, "z[3][1]");
        let v = 0;
        for (const row of z) v += row[0] * row[0];
        approx(v / 4, 1, 1e-12, "transformed column has unit variance");

        const back = await s.inverseTransform(z);
        for (let i = 0; i < 4; i++) approxArr(back[i], X[i], 1e-9, `inverse row ${i}`);
    } finally { s.close(); }
});

await __kitS.section("constant column std is 1", async () => {
    const s = new StandardScaler();
    try {
        await s.fit([[5, 1], [5, 2], [5, 3]]);
        approxArr(s.std, [1, Math.sqrt(2 / 3)], 1e-12, "constant column std is 1");
        const z = await s.transform([[5, 2]]);
        assert(z[0][0] === 0, "constant column transforms to exactly 0, not NaN");
        assert(Number.isFinite(z[0][1]), "other column still finite");
    } finally { s.close(); }
});

await __kitS.section("flat in -> Float64Array out", async () => {
    const flat = new Float64Array([1, 10, 2, 20, 3, 30, 4, 40]);
    const a = new StandardScaler(), b = new StandardScaler();
    try {
        const viaBoth = await (await a.fit(flat, 4, 2)).transform(flat, 4, 2);
        const viaOnce = await b.fitTransform(flat, 4, 2);
        assert(viaBoth instanceof Float64Array, "flat in -> Float64Array out");
        assert(viaOnce instanceof Float64Array, "fitTransform flat in -> flat out");
        assert(viaBoth.length === 8, "flat result length is rows*cols");
        approxArr(Array.from(viaOnce), Array.from(viaBoth), 0, "fitTransform === fit+transform");
        approxArr(a.mean, b.mean, 0, "same means");
    } finally { a.close(); b.close(); }
});

await __kitS.section("transform before fit", async () => {
    const s = new StandardScaler();
    try {
        await throws(async () => await s.transform([[1, 2]]), "transform before fit");
        await s.fit([[1, 2]]);
        await throws(async () => await s.transform([[1, 2, 3]]), "feature count mismatch", TypeError);
    } finally { s.close(); }
});

await __kitS.section("reentrant close during X read", async () => {
    const s = new StandardScaler();
    await s.fit([[1, 2], [3, 4]]);
    const bomb = [[{ valueOf() { s.close(); return 1; } }, 2]];
    await throws(async () => await s.transform(bomb), "reentrant close during X read");
    assert(s.closed === true, "the hostile valueOf did close it");
    await throws(async () => await s.transform([[1, 2]]), "use after close");
});

await __kitS.section("scaler keeps small spread on a large mean", async () => {
    const s = new StandardScaler();
    await s.fit([[1e12], [1e12 + 1]]);
    const t = await s.transform([[1e12], [1e12 + 1]]);
    approx(t[0][0], -1.0, 1e-12, "scaler keeps small spread on a large mean");
    approx(t[1][0], 1.0, 1e-12, "scaler keeps small spread on a large mean (hi)");
    await throws(async () => await new StandardScaler().fit([[2e200, 1], [0, 2]]),
           "scaler variance overflow", RangeError);
    await throws(async () => await meanSquaredError([0, 1], [NaN, 0.5]), "mse NaN refusal", RangeError);
});
__kitS.end();

print("test_ml_preprocessing: all tests passed (" + n + " assertions)");
