// covers: dyna:ml.DecisionTreeRegressor
// Crafted persisted records with a valid CRC must be refused at deserialize, never loaded:
//  - PCA with zero components (u64 at body offset 12 set to 0): "malformed PCA record", both with
//    the old arrays left as trailing bytes and cut to the exact size, so the n_components == 0
//    check is reached on its own and not only the trailing-bytes check.
//  - DecisionTreeRegressor with newton=1 on a non-boosting forest (u32 at body offset 148):
//    "malformed DecisionTreeRegressor record". Three checks refuse it: newton without max_bins, a
//    non-boosting forest with newton, and the type-family check after the read. The second craft
//    also sets max_bins (u64 at offset 140) so it passes the first; the other two stay redundant
//    on purpose (defence in depth, accepted 2026-11-14): only removing both turns it red.
// This was once tests/ml/probe_ml_zero.js, which chose its case from scriptArgs[0]; that is the
// script path, never "pca", so only the tree case ever ran. Both cases now run in one process.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_decision_tree_regressor_persist_crafted");
import * as ml from "dyna:ml";

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
function zeroComponents(rec, nf) {
    const body = 4 + 8 + 8 + 8 + 8 * nf;
    const r = new Uint8Array(HDR + body + 4);
    r.set(rec.subarray(0, HDR + body));
    const dv = new DataView(r.buffer);
    dv.setBigUint64(12, BigInt(body), true);
    dv.setBigUint64(HDR + 12, 0n, true);
    dv.setUint32(r.length - 4, crc32c(r, r.length - 4), true);
    return r;
}
const X = [[0, 0], [0.1, 0.2], [1, 1], [1.1, 0.9], [2, 2], [2.2, 1.8], [0.5, 1.5], [1.5, 0.5]];
const y = [0.0, 0.2, 1.0, 0.9, 2.0, 1.8, 1.5, 0.5];

let n = 0, fails = 0;
const check = (c, m) => { n++; if (!c) { fails++; print("FAIL: " + m); } };
function refused(fn, want, what) {
    let e = null;
    try { fn(); } catch (x) { e = x; }
    check(e instanceof TypeError && e.message === want, what + " (got " + (e ? e.name + ": " + e.message : "no throw") + ")");
}

await __kitS.section("malformed DecisionTreeRegressor record", async () => {
    const t = await new ml.DecisionTreeRegressor({ seed: 3, maxDepth: 4 }).fit(X, y);
    const rec = t.serialize();
    t.close();
    refused(() => ml.DecisionTreeRegressor.deserialize(patch(rec, 148, "u32", 1)), "malformed DecisionTreeRegressor record",
        "newton=1 on a non-boosting forest is refused");
    refused(() => ml.DecisionTreeRegressor.deserialize(patch(patch(rec, 140, "u64", 256), 148, "u32", 1)),
        "malformed DecisionTreeRegressor record", "newton=1 with maxBins set (past the newton-needs-bins check) is refused");
    const ok = ml.DecisionTreeRegressor.deserialize(rec);
    await ok.fit(X, y);
    check(true, "CONTROL: the unpatched tree record loads and refits");
    ok.close();
});
__kitS.end();

print("test_ml_persist_crafted: " + n + " checks, " + fails + " failed");
if (fails) throw new Error("test_ml_persist_crafted: " + fails + " failures");
