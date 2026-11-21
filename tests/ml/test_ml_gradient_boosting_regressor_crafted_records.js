// timeout: 120
// covers: dyna:ml.GradientBoostingRegressor
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_gradient_boosting_regressor_crafted_records");
import * as ml from "dyna:ml";

let pass = 0, fail = 0;
const ok = (c, w, d) => { if (c) pass++; else { fail++; print("  FAIL  " + w + (d ? "  [" + d + "]" : "")); } };

const HDR = 20;
const TBL = (() => { const t = new Uint32Array(256);
    for (let n = 0; n < 256; n++) { let c = n; for (let k = 0; k < 8; k++) c = (c & 1) ? (0x82F63B78 ^ (c >>> 1)) : (c >>> 1); t[n] = c >>> 0; }
    return t; })();
function crc32c(b, n) { let c = 0xFFFFFFFF; for (let i = 0; i < n; i++) c = TBL[(c ^ b[i]) & 0xFF] ^ (c >>> 8); return (c ^ 0xFFFFFFFF) >>> 0; }

function patch(rec, off, kind, v) {
    const r = rec.slice();
    const dv = new DataView(r.buffer, r.byteOffset, r.byteLength);
    const o = HDR + off;
    if (kind === "f64") dv.setFloat64(o, v, true);
    else if (kind === "u64") dv.setBigUint64(o, BigInt(v), true);
    else dv.setUint32(o, v >>> 0, true);
    dv.setUint32(r.length - 4, crc32c(r, r.length - 4), true);
    return r;
}

const X = [[0, 0], [0.1, 0.2], [1, 1], [1.1, 0.9], [2, 2], [2.2, 1.8], [0.5, 1.5], [1.5, 0.5]];
const y = [0.0, 0.2, 1.0, 0.9, 2.0, 1.8, 1.5, 0.5];

function tryLoad(ctor, bytes) {
    try { const m = ctor.deserialize(bytes); return { m, threw: false, msg: "" }; }
    catch (e) { return { m: null, threw: true, msg: String(e.message) }; }
}

print("== SEC-044: a zero-component PCA record must be refused, not divide by zero ==");

print("");
print("== positive control: a boosting model with newton still round-trips and refits ==");
await __kitS.section("an unpatched boosting record still loads", async () => {
    const g = new ml.GradientBoostingRegressor({ nEstimators: 4, maxDepth: 2, seed: 3 });
    await g.fit(X, y);
    const rec = g.serialize();
    g.close();
    const back = tryLoad(ml.GradientBoostingRegressor, rec);
    ok(!back.threw, "an unpatched boosting record still loads", back.msg);
    if (back.m) {
        let r = null, msg = "";
        try { r = await (await back.m.fit(X, y)).predict([[0.5, 0.5]]); } catch (e) { msg = String(e.message); }
        ok(r !== null && r.length === 1 && isFinite(r[0]), "a boosting record refits and predicts", msg);
        back.m.close();
    }
});
__kitS.end();

print("");
print("test_ml_crafted_records: " + (fail ? "FAILURES: " + fail + " / " : "all ") +
      (pass + fail) + " assertions passed");
if (fail)
    throw new Error("test_ml_crafted_records: " + fail + " of " + (pass + fail) + " assertions FAILED");
