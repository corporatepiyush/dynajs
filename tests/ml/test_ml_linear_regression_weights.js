// covers: dyna:ml.LinearRegression
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_linear_regression_weights");
import { LinearRegression, LogisticRegression, KMeans, PCA, GaussianNB, DecisionTreeClassifier, DecisionTreeRegressor, RandomForestRegressor, GradientBoostingRegressor, KNClassifier, KNRegressor, DBScan, StandardScaler, MinMaxScaler, SVC, GaussianMixture } from "dyna:ml";

let n = 0;
function assert(c, msg) { n++; if (!c) throw new Error("assertion failed: " + msg); }
function close(a, b, tol, msg) {
    n++;
    if (!(Math.abs(a - b) <= tol))
        throw new Error("assertion failed: " + msg + " (" + a + " vs " + b + ")");
}
async function throwsWith(fn, needle, msg) {
    n++;
    let e = null;
    try { await fn(); } catch (err) { e = err; }
    if (e === null) throw new Error("assertion failed: " + msg + " (expected a throw)");
    if (!String(e.message).includes(needle))
        throw new Error("assertion failed: " + msg + " (message was: " + e.message + ")");
}

await __kitS.section("all-ones is BIT-identical to no weights, not merely close", async () => {
    const X = [[1], [2], [3], [4]], y = [1, 2, 3, 10];
    const at = async (m) => (await m.predict([[10]]))[0];

    const plain = await new LinearRegression().fit(X, y);
    const ones = await new LinearRegression().fit(X, y, { sampleWeight: [1, 1, 1, 1] });
    assert((await at(plain)) === (await at(ones)),
        "all-ones is BIT-identical to no weights, not merely close");

    const zeroed = await new LinearRegression().fit(X, y, { sampleWeight: [1, 1, 1, 0] });
    const deleted = await new LinearRegression().fit([[1], [2], [3]], [1, 2, 3]);
    close(await at(zeroed), await at(deleted), 1e-7, "weight 0 == the row not being there");
    assert(Math.abs((await at(zeroed)) - (await at(plain))) > 10,
        "and it really is a different fit from the unweighted one");

    const dup = await new LinearRegression().fit([[1], [2], [3], [3]], [1, 2, 3, 3]);
    const wt = await new LinearRegression().fit([[1], [2], [3]], [1, 2, 3],
                                          { sampleWeight: [1, 1, 2] });
    close(await at(dup), await at(wt), 1e-7, "weight 2 == the row appearing twice");

    const scaled = await new LinearRegression().fit(X, y, { sampleWeight: [7, 7, 7, 7] });
    close(await at(scaled), await at(plain), 1e-7, "a uniform scale factor is a no-op");

    const X2 = [[1, 0], [2, 1], [3, 0], [4, 1]], y2 = [2, 5, 6, 11];
    const d2 = await new LinearRegression().fit([[1, 0], [2, 1], [2, 1], [3, 0], [4, 1]],
                                          [2, 5, 5, 6, 11]);
    const w2 = await new LinearRegression().fit(X2, y2, { sampleWeight: [1, 2, 1, 1] });
    close((await d2.predict([[2, 1]]))[0], (await w2.predict([[2, 1]]))[0], 1e-7,
        "multi-feature weight 2 == a duplicated row");
});

await __kitS.section("one entry per row", async () => {
    const X = [[1], [2], [3], [4]], y = [1, 2, 3, 10];
    const fit = async (w) => await new LinearRegression().fit(X, y, { sampleWeight: w });

    await throwsWith(async () => await fit([1, 1, 1]), "one entry per row", "a short vector is refused");
    await throwsWith(async () => await fit([1, 1, 1, 1, 1]), "one entry per row", "a long vector is refused");
    await throwsWith(async () => await fit([1, -1, 1, 1]), "negative", "a negative weight is refused, and named");
    await throwsWith(async () => await fit([1, NaN, 1, 1]), "NaN", "NaN is refused, and named");
    await throwsWith(async () => await fit([1, Infinity, 1, 1]), "infinite", "infinity is refused, and named");
    await throwsWith(async () => await fit([0, 0, 0, 0]), "sums to zero", "all-zero has nothing to fit");

    assert(Number.isFinite((await (await fit([0, 1, 1, 1])).predict([[2]]))[0]),
        "one zero weight is legal");

    const plain = (await (await new LinearRegression().fit(X, y)).predict([[10]]))[0];
    for (const w of [undefined, null]) {
        n++;
        const got = (await (await new LinearRegression().fit(X, y, { sampleWeight: w })).predict([[10]]))[0];
        if (got !== plain) throw new Error("sampleWeight: " + w + " must mean 'none'");
    }
    let cc6 = null;
    try { await new LinearRegression().fit(X, y, { somethingElse: 1 }); }
    catch (e) { cc6 = e; }
    assert(cc6 !== null && cc6 instanceof TypeError &&
           /unknown option "somethingElse" \(valid: sampleWeight\)/.test(cc6.message),
        "unknown fit() key throws naming key + valid set (got " +
        (cc6 && cc6.message) + ")");
    assert((await (await new LinearRegression().fit(X, y, {})).predict([[10]]))[0] === plain,
        "an empty options object still means no weights");
});

await __kitS.section("weighted fit must refuse a detached X, got:", async () => {
    const rows = 500, cols = 2;
    const X = new Float64Array(rows * cols);
    const y = new Float64Array(rows);
    for (let i = 0; i < rows; i++) {
        const a = Math.sin(i * 0.1) * 3, b = Math.cos(i * 0.05) * 2;
        X[i * 2] = a; X[i * 2 + 1] = b;
        y[i] = 2 * a - b + 0.05 * Math.sin(i);
    }
    const w = new Float64Array(rows);
    for (let i = 0; i < rows; i++) w[i] = (i % 3) + 1;

    let usedPrimitive = "ArrayBuffer.prototype.transfer";
    const makeDetachingOpts = () => {
        let fired = false;
        return {
            get sampleWeight() {
                if (!fired) {
                    fired = true;
                    X.buffer.transfer();
                }
                return w;
            }
        };
    };

    let outcome = null;
    try {
        const m = await new LinearRegression().fit(X, y, rows, cols,
                                             makeDetachingOpts());
        outcome = { threw: false, coef: m.coef };
    } catch (e) {
        outcome = { threw: true, msg: String(e.message) };
    }

    if (outcome.threw) {
        n++;
        if (!/detached/i.test(outcome.msg))
            throw new Error("weighted fit must refuse a detached X, got: "
                            + outcome.msg);
    } else {
        n++;
        if (!(Math.abs(outcome.coef[0] - 2) < 0.1 &&
              Math.abs(outcome.coef[1] + 1) < 0.1))
            throw new Error(
                "weighted fit returned silent wrong coefficients after a "
                + "detached X: " + outcome.coef);
    }
    print("  P0-2 regression (" + usedPrimitive + "): "
          + (outcome.threw ? "clean throw '" + outcome.msg + "'"
                           : "correct coefficients " + outcome.coef));

    const X2 = new Float64Array(rows * cols);
    for (let i = 0; i < rows; i++) {
        X2[i * 2] = Math.sin(i * 0.1) * 3;
        X2[i * 2 + 1] = Math.cos(i * 0.05) * 2;
    }
    n++;
    const clean = await new LinearRegression().fit(X2, y, rows, cols,
                                             { sampleWeight: w });
    if (!(Math.abs(clean.coef[0] - 2) < 0.1 &&
          Math.abs(clean.coef[1] + 1) < 0.1))
        throw new Error("weighted fit without detach regressed: "
                        + clean.coef);
});
__kitS.end();

print("test_ml_weights: all " + n + " assertions passed");
