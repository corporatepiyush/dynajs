// covers: dyna:ml.accuracy
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_accuracy_preprocessing");
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

await __kitS.section("accuracy 3/4", async () => {
    approx(await accuracy([0, 1, 1, 0], [0, 1, 0, 0]), 0.75, 1e-12, "accuracy 3/4");
    approx(await accuracy([1, 1], [0, 0]), 0, 0, "accuracy 0");
    approx(await accuracy([1, 1], [1, 1]), 1, 0, "accuracy 1");

    approx(await logLoss([1, 0], [0.9, 0.1]), -(Math.log(0.9) + Math.log(0.9)) / 2, 1e-12,
           "logLoss symmetric case");
    const worst = await logLoss([1], [0]);
    assert(Number.isFinite(worst) && worst > 30, "logLoss of a confident miss is large but finite");
    approx(await logLoss([1], [1]), 0, 1e-9, "logLoss of a confident hit is ~0");

    const cm = await confusionMatrix([0, 1, 2, 2, 0], [0, 1, 2, 0, 1]);
    assert(cm.length === 3 && cm[0].length === 3, "3x3 for labels 0..2");
    approxArr(cm[0], [1, 1, 0], 0, "true class 0 row");
    approxArr(cm[1], [0, 1, 0], 0, "true class 1 row");
    approxArr(cm[2], [1, 0, 1], 0, "true class 2 row");
    const cm2 = await confusionMatrix([0, 0], [0, 3]);
    assert(cm2.length === 4, "sized by the largest label in EITHER vector");
    approx(cm2[0][3], 1, 0, "off-diagonal for the unseen true class");

    await throws(async () => await confusionMatrix([0, 1], [0, -1]), "negative label", RangeError);
    await throws(async () => await confusionMatrix([0, 1], [0, 1.5]), "non-integer label", RangeError);
    await throws(async () => await confusionMatrix([0], [9999]), "label beyond the cap", RangeError);
});
__kitS.end();

print("test_ml_preprocessing: all tests passed (" + n + " assertions)");
