// timeout: 120
// covers: dyna:ml.DecisionTreeRegressor
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_decision_tree_regressor_crafted_records");
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
print("== SEC-045: a non-boosting forest record with newton=1 must be refused ==");
await __kitS.section("newton=1 with boosting=0 is refused by the loader", async () => {
    const mk = async () => await new ml.DecisionTreeRegressor({ seed: 3, maxDepth: 4 }).fit(X, y);
    const t = await mk();
    const rec = t.serialize();
    t.close();
    const NEWTON = 148;
    const forged = tryLoad(ml.DecisionTreeRegressor, patch(rec, NEWTON, "u32", 1));
    ok(forged.threw, "newton=1 with boosting=0 is refused by the loader");
    ok(/forest|malformed|not a|record|XGB|DecisionTree/.test(forged.msg),
       "newton=1 refusal names the record", forged.msg.slice(0, 60));
    if (forged.m) {
        let seg = false, msg = "";
        try { await forged.m.fit(X, y); seg = true; } catch (e) { msg = String(e.message); }
        ok(!seg, "a forged newton=1 model must not reach dyn_boost_init_base with NULL pred/raw_base", msg.slice(0, 60));
        forged.m.close();
    }
});
__kitS.end();

print("");
print("test_ml_crafted_records: " + (fail ? "FAILURES: " + fail + " / " : "all ") +
      (pass + fail) + " assertions passed");
if (fail)
    throw new Error("test_ml_crafted_records: " + fail + " of " + (pass + fail) + " assertions FAILED");
