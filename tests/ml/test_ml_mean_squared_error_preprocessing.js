// covers: dyna:ml.meanSquaredError
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_mean_squared_error_preprocessing");
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

await __kitS.section("length mismatch", async () => {
    await throws(async () => await meanSquaredError([1, 2], [1]), "length mismatch", TypeError);
    await throws(async () => await meanSquaredError([], []), "empty input", RangeError);
    await throws(async () => await accuracy([1, 2]), "missing second argument", TypeError);
});
__kitS.end();

print("test_ml_preprocessing: all tests passed (" + n + " assertions)");
