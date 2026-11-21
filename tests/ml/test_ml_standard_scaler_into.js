// covers: dyna:ml.StandardScaler
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_standard_scaler_into");
import * as ml from "dyna:ml";

let n = 0;
function assert(c, m) { n++; if (!c) throw new Error("assert: " + m); }
async function throws(fn, m) { let t = false; try { await fn(); } catch { t = true; } assert(t, m); }
function eqArr(a, b) { if (a.length !== b.length) return false;
    for (let i = 0; i < a.length; i++) if (!Object.is(a[i], b[i])) return false; return true; }
function mulberry32(seed) { let a = seed >>> 0; return function () {
    a |= 0; a = (a + 0x6D2B79F5) | 0; let t = Math.imul(a ^ (a >>> 15), 1 | a);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t; return ((t ^ (t >>> 14)) >>> 0) / 4294967296; }; }

await __kitS.section("scaler transformInto row count", async () => {
    const X = [[0, 10], [1, 11], [2, 12], [3, 13]];
    const sc = new ml.StandardScaler();
    await sc.fit(X);
    const want = await sc.transform(X);
    const out = new Float64Array(X.length * 2);
    assert((await sc.transformInto(out, X)) === X.length, "scaler transformInto row count");
    assert(eqArr([...out], [...want.flat()]), "scaler transformInto == transform");
    const flat = new Float64Array([0, 10, 1, 11, 2, 12, 3, 13]);
    const out2 = new Float64Array(8);
    await sc.transformInto(out2, flat, 4, 2);
    assert(eqArr([...out2], [...want.flat()]), "scaler transformInto flat form");
    const mm = new ml.MinMaxScaler();
    await mm.fit(X);
    const w2 = await mm.transform(X);
    const out3 = new Float64Array(8);
    await mm.transformInto(out3, X);
    assert(eqArr([...out3], [...w2.flat()]), "minmax transformInto == transform");
    const pca = new ml.PCA({ nComponents: 2 });
    await pca.fit(X);
    const w3 = await pca.transform(X);
    const out4 = new Float64Array(X.length * 2);
    assert((await pca.transformInto(out4, X)) === X.length, "pca transformInto row count");
    assert(eqArr([...out4], [...w3.flat()]), "pca transformInto == transform");
    await throws(async () => await sc.transformInto(new Float64Array(2), X), "transformInto short out");
    await throws(async () => await sc.transformInto([0, 0], X), "transformInto wrong type");
    const fresh = new ml.StandardScaler();
    await throws(async () => await fresh.transformInto(new Float64Array(8), X), "transformInto before fit");
});
__kitS.end();

console.log(`test_ml_into: ${n} assertions ok`);
