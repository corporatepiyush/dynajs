// covers: dyna:ml.imputeMean
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_impute_mean_production");
import * as ml from "dyna:ml";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}
function eq(got, want, msg) {
    n++;
    if (got !== want)
        throw new Error("assertion failed: " + msg + "\n  got:  " + got +
                        "\n  want: " + want);
}
function near(got, want, tol, msg) {
    n++;
    if (!(Math.abs(got - want) <= tol))
        throw new Error("assertion failed: " + msg + "\n  got:  " + got +
                        "\n  want: " + want);
}
async function throws(fn, msg) {
    n++;
    try { await fn(); } catch (e) { return e; }
    throw new Error("assertion failed: " + msg + " did not throw");
}

function rng(seed) {
    let s = seed >>> 0;
    return () => { s = (Math.imul(s, 1103515245) + 12345) >>> 0; return s / 4294967296; };
}
function blobs(rows, k, seed, spread) {
    const r = rng(seed), X = [], y = [];
    for (let i = 0; i < rows; i++) {
        const c = i % k;
        X.push([c * spread + r() * 2, c * spread + r() * 2, r()]);
        y.push(c);
    }
    return [X, y];
}

await __kitS.section("column 0 mean fills the NaN", async () => {
    const X = [[1, 10], [NaN, 20], [3, NaN], [5, 40]];
    const out = await ml.imputeMean(X);
    near(out[1][0], (1 + 3 + 5) / 3, 1e-12, "column 0 mean fills the NaN");
    near(out[2][1], (10 + 20 + 40) / 3, 1e-12, "column 1 mean fills the NaN");
    eq(out[0][0], 1, "finite entries are untouched");
    eq(out[3][1], 40, "and so are the last ones");
    assert(Number.isNaN(X[1][0]), "the caller's array is unchanged");
    await throws(async () => await ml.imputeMean([[NaN, 1], [NaN, 2]]), "an all-NaN column");
    const y = [1, 2, 3, 4];
    await new ml.LinearRegression().fit(await ml.imputeMean(X), y);
    assert(true, "imputed data is accepted by fit");
});
__kitS.end();
console.log("test_ml_production.js: " + n + " assertions passed");
