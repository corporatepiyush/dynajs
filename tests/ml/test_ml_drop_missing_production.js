// covers: dyna:ml.dropMissing
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_drop_missing_production");
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

await __kitS.section("rows dropped", async () => {
    const X = [[1, 2], [3, NaN], [5, 6], [NaN, 8], [9, 10]];
    const y = [1, 2, 3, 4, 5];
    const d = await ml.dropMissing(X, y);
    eq(JSON.stringify(d.X), JSON.stringify([[1, 2], [5, 6], [9, 10]]), "rows dropped");
    eq(JSON.stringify(Array.from(d.y)), JSON.stringify([1, 3, 5]), "y follows X");
    eq(JSON.stringify(d.kept), JSON.stringify([0, 2, 4]),
       "kept is the surviving INDICES, so other columns can follow too");
    const d2 = await ml.dropMissing([[1], [2], [3]], [1, NaN, 3]);
    eq(JSON.stringify(d2.kept), JSON.stringify([0, 2]), "a NaN target drops its row");
    const d3 = await ml.dropMissing(X);
    eq(d3.y, undefined, "y is undefined when none was given (d.ts: Float64Array | undefined)");
    eq(d3.X.length, 3, "and X is still filtered");
    await new ml.LinearRegression().fit(d.X, d.y);
    assert(true, "dropMissing output is accepted by fit");
});
__kitS.end();
console.log("test_ml_production.js: " + n + " assertions passed");
