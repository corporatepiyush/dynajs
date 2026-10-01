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
function throws(fn, msg, ErrType) {
    n++;
    try { fn(); } catch (e) {
        if (ErrType && !(e instanceof ErrType))
            throw new Error(`${msg}: wrong error type ${e.constructor.name}`);
        return;
    }
    throw new Error("expected a throw: " + msg);
}

{
    const X = [[1, 10], [2, 20], [3, 30], [4, 40]];
    const s = new StandardScaler();
    try {
        assert(s.fit(X) === s, "fit returns this");
        approxArr(s.mean, [2.5, 25], 1e-12, "mean");
        approxArr(s.std, [Math.sqrt(1.25), Math.sqrt(125)], 1e-12, "population std (ddof=0)");

        const z = s.transform(X);
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

        const back = s.inverseTransform(z);
        for (let i = 0; i < 4; i++) approxArr(back[i], X[i], 1e-9, `inverse row ${i}`);
    } finally { s.close(); }
}
{
    const s = new StandardScaler();
    try {
        s.fit([[5, 1], [5, 2], [5, 3]]);
        approxArr(s.std, [1, Math.sqrt(2 / 3)], 1e-12, "constant column std is 1");
        const z = s.transform([[5, 2]]);
        assert(z[0][0] === 0, "constant column transforms to exactly 0, not NaN");
        assert(Number.isFinite(z[0][1]), "other column still finite");
    } finally { s.close(); }
}
{
    const flat = new Float64Array([1, 10, 2, 20, 3, 30, 4, 40]);
    const a = new StandardScaler(), b = new StandardScaler();
    try {
        const viaBoth = a.fit(flat, 4, 2).transform(flat, 4, 2);
        const viaOnce = b.fitTransform(flat, 4, 2);
        assert(viaBoth instanceof Float64Array, "flat in -> Float64Array out");
        assert(viaOnce instanceof Float64Array, "fitTransform flat in -> flat out");
        assert(viaBoth.length === 8, "flat result length is rows*cols");
        approxArr(Array.from(viaOnce), Array.from(viaBoth), 0, "fitTransform === fit+transform");
        approxArr(a.mean, b.mean, 0, "same means");
    } finally { a.close(); b.close(); }
}
{
    const s = new StandardScaler();
    try {
        throws(() => s.transform([[1, 2]]), "transform before fit");
        s.fit([[1, 2]]);
        throws(() => s.transform([[1, 2, 3]]), "feature count mismatch", TypeError);
    } finally { s.close(); }
}

{
    const X = [[1, -10], [3, 0], [5, 10]];
    const s = new MinMaxScaler();
    try {
        s.fit(X);
        approxArr(s.dataMin, [1, -10], 1e-12, "dataMin");
        approxArr(s.dataMax, [5, 10], 1e-12, "dataMax");
        const t = s.transform(X);
        approxArr(t[0], [0, 0], 1e-12, "min row maps to 0");
        approxArr(t[1], [0.5, 0.5], 1e-12, "middle row maps to 0.5");
        approxArr(t[2], [1, 1], 1e-12, "max row maps to 1");
        approx(s.transform([[7, 0]])[0][0], 1.5, 1e-12, "beyond max extrapolates");
        const back = s.inverseTransform(t);
        for (let i = 0; i < 3; i++) approxArr(back[i], X[i], 1e-9, `inverse row ${i}`);
    } finally { s.close(); }
}
{
    const s = new MinMaxScaler();
    try {
        s.fit([[7, 1], [7, 5]]);
        approxArr(s.dataMax, [7, 5], 1e-12, "dataMax with a constant column");
        const t = s.transform([[7, 3]]);
        assert(t[0][0] === 0, "constant column -> 0, not NaN");
        approx(t[0][1], 0.5, 1e-12, "other column unaffected");
    } finally { s.close(); }
}

{
    const yt = [3, -0.5, 2, 7];
    const yp = [2.5, 0.0, 2, 8];
    approx(meanSquaredError(yt, yp), 1.5 / 4, 1e-12, "mse (sklearn doc example)");
    approx(meanAbsoluteError(yt, yp), 2.0 / 4, 1e-12, "mae (sklearn doc example)");
    approx(r2Score(yt, yp), 0.9486081370449679, 1e-12, "r2 (sklearn doc example)");

    approx(meanSquaredError(yt, yt), 0, 0, "mse of a perfect fit is 0");
    approx(r2Score(yt, yt), 1, 1e-12, "r2 of a perfect fit is 1");

    approx(r2Score([2, 2, 2], [2, 2, 2]), 1, 0, "r2 constant y, exact -> 1");
    approx(r2Score([2, 2, 2], [1, 2, 3]), 0, 0, "r2 constant y, inexact -> 0");
    assert(!Number.isNaN(r2Score([2, 2, 2], [1, 2, 3])), "r2 never NaN");

    approx(meanSquaredError(new Float64Array(yt), new Float64Array(yp)),
           meanSquaredError(yt, yp), 0, "Float64Array === Array input");
}

{
    approx(accuracy([0, 1, 1, 0], [0, 1, 0, 0]), 0.75, 1e-12, "accuracy 3/4");
    approx(accuracy([1, 1], [0, 0]), 0, 0, "accuracy 0");
    approx(accuracy([1, 1], [1, 1]), 1, 0, "accuracy 1");

    approx(logLoss([1, 0], [0.9, 0.1]), -(Math.log(0.9) + Math.log(0.9)) / 2, 1e-12,
           "logLoss symmetric case");
    const worst = logLoss([1], [0]);
    assert(Number.isFinite(worst) && worst > 30, "logLoss of a confident miss is large but finite");
    approx(logLoss([1], [1]), 0, 1e-9, "logLoss of a confident hit is ~0");

    const cm = confusionMatrix([0, 1, 2, 2, 0], [0, 1, 2, 0, 1]);
    assert(cm.length === 3 && cm[0].length === 3, "3x3 for labels 0..2");
    approxArr(cm[0], [1, 1, 0], 0, "true class 0 row");
    approxArr(cm[1], [0, 1, 0], 0, "true class 1 row");
    approxArr(cm[2], [1, 0, 1], 0, "true class 2 row");
    const cm2 = confusionMatrix([0, 0], [0, 3]);
    assert(cm2.length === 4, "sized by the largest label in EITHER vector");
    approx(cm2[0][3], 1, 0, "off-diagonal for the unseen true class");

    throws(() => confusionMatrix([0, 1], [0, -1]), "negative label", RangeError);
    throws(() => confusionMatrix([0, 1], [0, 1.5]), "non-integer label", RangeError);
    throws(() => confusionMatrix([0], [9999]), "label beyond the cap", RangeError);
}
{
    throws(() => meanSquaredError([1, 2], [1]), "length mismatch", TypeError);
    throws(() => meanSquaredError([], []), "empty input", RangeError);
    throws(() => accuracy([1, 2]), "missing second argument", TypeError);
}

{
    const s = new StandardScaler();
    s.fit([[1, 2], [3, 4]]);
    const bomb = [[{ valueOf() { s.close(); return 1; } }, 2]];
    throws(() => s.transform(bomb), "reentrant close during X read");
    assert(s.closed === true, "the hostile valueOf did close it");
    throws(() => s.transform([[1, 2]]), "use after close");
}
{
    const s = new MinMaxScaler();
    const bomb = [[{ valueOf() { s.close(); return 1; } }]];
    throws(() => s.fit(bomb), "reentrant close during fit");
    assert(s.closed === true, "closed");
}

{
    const s = new StandardScaler();
    s.fit([[1e12], [1e12 + 1]]);
    const t = s.transform([[1e12], [1e12 + 1]]);
    approx(t[0][0], -1.0, 1e-12, "scaler keeps small spread on a large mean");
    approx(t[1][0], 1.0, 1e-12, "scaler keeps small spread on a large mean (hi)");
    throws(() => new StandardScaler().fit([[2e200, 1], [0, 2]]),
           "scaler variance overflow", RangeError);
    throws(() => meanSquaredError([0, 1], [NaN, 0.5]), "mse NaN refusal", RangeError);
}

print("test_ml_preprocessing: all tests passed (" + n + " assertions)");
