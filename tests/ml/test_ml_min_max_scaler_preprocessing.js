// covers: dyna:ml.MinMaxScaler
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_min_max_scaler_preprocessing");
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

await __kitS.section("min row maps to 0", async () => {
    const X = [[1, -10], [3, 0], [5, 10]];
    const s = new MinMaxScaler();
    try {
        await s.fit(X);
        approxArr(s.dataMin, [1, -10], 1e-12, "dataMin");
        approxArr(s.dataMax, [5, 10], 1e-12, "dataMax");
        const t = await s.transform(X);
        approxArr(t[0], [0, 0], 1e-12, "min row maps to 0");
        approxArr(t[1], [0.5, 0.5], 1e-12, "middle row maps to 0.5");
        approxArr(t[2], [1, 1], 1e-12, "max row maps to 1");
        approx((await s.transform([[7, 0]]))[0][0], 1.5, 1e-12, "beyond max extrapolates");
        const back = await s.inverseTransform(t);
        for (let i = 0; i < 3; i++) approxArr(back[i], X[i], 1e-9, `inverse row ${i}`);
    } finally { s.close(); }
});

await __kitS.section("dataMax with a constant column", async () => {
    const s = new MinMaxScaler();
    try {
        await s.fit([[7, 1], [7, 5]]);
        approxArr(s.dataMax, [7, 5], 1e-12, "dataMax with a constant column");
        const t = await s.transform([[7, 3]]);
        assert(t[0][0] === 0, "constant column -> 0, not NaN");
        approx(t[0][1], 0.5, 1e-12, "other column unaffected");
    } finally { s.close(); }
});

await __kitS.section("reentrant close during fit", async () => {
    const s = new MinMaxScaler();
    const bomb = [[{ valueOf() { s.close(); return 1; } }]];
    await throws(async () => await s.fit(bomb), "reentrant close during fit");
    assert(s.closed === true, "closed");
});
__kitS.end();

print("test_ml_preprocessing: all tests passed (" + n + " assertions)");
