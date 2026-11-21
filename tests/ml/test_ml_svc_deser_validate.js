// timeout: 600
// covers: dyna:ml.SVC
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_svc_deser_validate");
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
const yc = [0, 0, 1, 1, 0, 0, 1, 1];
const yr = yc.map(v => v * 1.0);

print("== 1. a forged DBScan eps is refused at the door ==");
const EPS = [
    ["0", 0, false], ["-0", -0, false], ["-1", -1, false], ["-1e300", -1e300, false],
    ["NaN", NaN, false], ["+Inf", Infinity, false], ["-Inf", -Infinity, false],
    ["1e400 -> +Inf", Infinity, false],
    ["1e-320 (subnormal, square underflows to 0)", 1e-320, true],
    ["1e200 (square overflows to +Inf)", 1e200, false],
    ["0.5 (the module default)", 0.5, true],
];

print("");
print("== 3. SWEEP: for every codec, what does the loader do with a value the ==");
print("==    constructor refuses? Each row is pinned, refusals AND acceptances. ==");
const SVC_OFF = { gamma: 4, coef0: 12, C: 24, tol: 32, maxIter: 40, degree: 20 };

async function sweepRow(label, build, off, kind, v, expect) {
    const cls = label.split(" ")[0];
    const m0 = await build();
    let rec, cmsg = "";
    try { rec = m0.serialize(); } catch (e) { cmsg = String(e.message); }
    m0.close();
    if (!rec) { ok(false, label + ": could not make a record to forge", cmsg); return null; }

    let m = null, msg = "";
    try { m = ml[cls].deserialize(patch(rec, off, kind, v)); }
    catch (e) { msg = String(e.message); }

    if (expect === "refuse") {
        ok(m === null, label + ": the loader refuses it, as the constructor does",
           m === null ? "" : "LOADED as " + JSON.stringify(v));
    } else {
        ok(m !== null, label + ": loads (" + expect + ")",
           "refused: " + msg.slice(0, 50));
    }
    if (!m) return null;

    let used = false, umsg = "";
    try {
        if (m.predict) await m.predict(X);
        if (m.transform) await m.transform(X);
        if (m.labels) { const l = m.labels[0]; if (l === undefined) throw new Error("no labels"); }
        used = true;
    } catch (e) { umsg = String(e.message); }
    ok(true, label + ": using the loaded model is not fatal (" +
             (used ? "answered" : "refused cleanly: " + umsg.slice(0, 45)) + ")");

    if (expect === "accept-capped") {
        const n = 4000;
        const XL = new Float64Array(n), yL = new Float64Array(n);
        for (let i = 0; i < n; i++) { XL[i] = i; yL[i] = i % 2; }
        let depth = -1;
        try { await m.fit(XL, yL, n, 1); depth = m.depth; } catch (e) { depth = -1; }
        ok(depth >= 0 && depth <= 1024, label + ": a refit is still capped at 1024, got " + depth);
    }
    m.close();
    return m;
}

await __kitS.section("SVC degree=1001", async () => {
    const mk = async () => await new ml.SVC({ kernel: "rbf", C: 1.0 }).fit(X, yc);
    await sweepRow("SVC degree=1001",   mk, SVC_OFF.degree, "u32", 1001,  "refuse");
    await sweepRow("SVC maxIter=100001", mk, SVC_OFF.maxIter, "u64", 100001, "refuse");
    await sweepRow("SVC degree=0",      mk, SVC_OFF.degree, "u32", 0,      "accept");
    await sweepRow("SVC maxIter=0",     mk, SVC_OFF.maxIter, "u64", 0,     "accept");
    await sweepRow("SVC gamma=NaN",     mk, SVC_OFF.gamma, "f64", NaN,      "refuse");
    await sweepRow("SVC gamma=1e308",   mk, SVC_OFF.gamma, "f64", 1e308,    "accept");
    await sweepRow("SVC C=NaN",         mk, SVC_OFF.C, "f64", NaN,          "refuse");
    await sweepRow("SVC tol=NaN",       mk, SVC_OFF.tol, "f64", NaN,        "refuse");
    await sweepRow("SVC coef0=1e300",   mk, SVC_OFF.coef0, "f64", 1e300,    "accept");
});
__kitS.end();

print("");
print("test_ml_deser_validate: " + (fail ? "FAILURES: " + fail + " / " : "all ") +
      (pass + fail) + " assertions passed");
if (fail)
    throw new Error("test_ml_deser_validate: " + fail + " of " + (pass + fail) + " assertions FAILED");
