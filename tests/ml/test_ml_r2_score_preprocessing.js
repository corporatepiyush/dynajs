// covers: dyna:ml.r2Score
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_r2_score_preprocessing");
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

await __kitS.section("mse (sklearn doc example)", async () => {
    const yt = [3, -0.5, 2, 7];
    const yp = [2.5, 0.0, 2, 8];
    approx(await meanSquaredError(yt, yp), 1.5 / 4, 1e-12, "mse (sklearn doc example)");
    approx(await meanAbsoluteError(yt, yp), 2.0 / 4, 1e-12, "mae (sklearn doc example)");
    approx(await r2Score(yt, yp), 0.9486081370449679, 1e-12, "r2 (sklearn doc example)");

    approx(await meanSquaredError(yt, yt), 0, 0, "mse of a perfect fit is 0");
    approx(await r2Score(yt, yt), 1, 1e-12, "r2 of a perfect fit is 1");

    approx(await r2Score([2, 2, 2], [2, 2, 2]), 1, 0, "r2 constant y, exact -> 1");
    approx(await r2Score([2, 2, 2], [1, 2, 3]), 0, 0, "r2 constant y, inexact -> 0");
    assert(!Number.isNaN(await r2Score([2, 2, 2], [1, 2, 3])), "r2 never NaN");

    approx(await meanSquaredError(new Float64Array(yt), new Float64Array(yp)),
           await meanSquaredError(yt, yp), 0, "Float64Array === Array input");
});
__kitS.end();

print("test_ml_preprocessing: all tests passed (" + n + " assertions)");
