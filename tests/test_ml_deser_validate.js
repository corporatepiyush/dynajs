// timeout: 600
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
{
    const base = new ml.DBScan(0.3, 2).fit(X);
    const rec = base.serialize();
    base.close();
    for (const [label, v, wantLoad] of EPS) {
        let m = null, msg = "";
        try { m = ml.DBScan.deserialize(patch(rec, 0, "f64", v)); }
        catch (e) { msg = String(e.message); }
        if (wantLoad) {
            ok(m !== null, "forged eps=" + label + " still loads (the constructor accepts it)", msg);
            if (m) {
                let fitted = false, fmsg = "";
                try { m.fit(X); fitted = true; } catch (e) { fmsg = String(e.message); }
                ok(fitted, "forged eps=" + label + " refits without undefined behaviour", fmsg.slice(0, 50));
                m.close();
            }
        } else {
            ok(m === null, "forged eps=" + label + " is refused by the loader",
               m === null ? "" : "LOADED, eps now " + m.eps);
            if (m) m.close();
        }
    }
}

print("");
print("== 2. the same value through the CONSTRUCTOR, for the record ==");
for (const [label, v] of [["0", 0], ["-0", -0], ["-1", -1], ["NaN", NaN],
                           ["+Inf", Infinity], ["-Inf", -Infinity]]) {
    let ctorOk = false, cmsg = "";
    try { const m = new ml.DBScan(v, 2); m.close(); ctorOk = true; }
    catch (e) { cmsg = String(e.message); }
    ok(!ctorOk, "new DBScan(" + label + ") is refused", "accepted");
    ok(/eps must be positive/.test(cmsg), "new DBScan(" + label + ") names eps", cmsg.slice(0, 50));
}

print("");
print("== 3. SWEEP: for every codec, what does the loader do with a value the ==");
print("==    constructor refuses? Each row is pinned, refusals AND acceptances. ==");
const SVC_OFF = { gamma: 4, coef0: 12, C: 24, tol: 32, maxIter: 40, degree: 20 };
const GMM_OFF = { maxIter: 16, tol: 24, reg: 32 };
const LOGREG_OFF = { lr: 24, l1: 32, l2: 40, tol: 48, maxIter: 56 };
const FOREST_OFF = { maxDepth: 108, minSamplesSplit: 116, minSamplesLeaf: 124,
                     maxFeatures: 132, maxBins: 140, nEstimatorsIsDerived: 0 };

function sweepRow(label, build, off, kind, v, expect) {
    const cls = label.split(" ")[0];
    const m0 = build();
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
        if (m.predict) m.predict(X);
        if (m.transform) m.transform(X);
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
        try { m.fit(XL, yL, n, 1); depth = m.depth; } catch (e) { depth = -1; }
        ok(depth >= 0 && depth <= 1024, label + ": a refit is still capped at 1024, got " + depth);
    }
    m.close();
    return m;
}

{
    const mk = () => new ml.DBScan(0.3, 2).fit(X);
    sweepRow("DBScan eps=0",          mk, 0, "f64", 0,         "refuse");
    sweepRow("DBScan eps=-0",         mk, 0, "f64", -0,        "refuse");
    sweepRow("DBScan eps=NaN",        mk, 0, "f64", NaN,       "refuse");
    sweepRow("DBScan eps=+Inf",       mk, 0, "f64", Infinity,  "refuse");
    sweepRow("DBScan eps=-Inf",       mk, 0, "f64", -Infinity, "refuse");
    sweepRow("DBScan eps=-2.5",       mk, 0, "f64", -2.5,      "refuse");
    sweepRow("DBScan eps=1e-320",     mk, 0, "f64", 1e-320,    "accept");
}
{
    const mk = () => new ml.SVC({ kernel: "rbf", C: 1.0 }).fit(X, yc);
    sweepRow("SVC degree=1001",   mk, SVC_OFF.degree, "u32", 1001,  "refuse");
    sweepRow("SVC maxIter=100001", mk, SVC_OFF.maxIter, "u64", 100001, "refuse");
    sweepRow("SVC degree=0",      mk, SVC_OFF.degree, "u32", 0,      "accept");
    sweepRow("SVC maxIter=0",     mk, SVC_OFF.maxIter, "u64", 0,     "accept");
    sweepRow("SVC gamma=NaN",     mk, SVC_OFF.gamma, "f64", NaN,      "accept");
    sweepRow("SVC gamma=1e308",   mk, SVC_OFF.gamma, "f64", 1e308,    "accept");
    sweepRow("SVC C=NaN",         mk, SVC_OFF.C, "f64", NaN,          "accept");
    sweepRow("SVC tol=NaN",       mk, SVC_OFF.tol, "f64", NaN,        "accept");
    sweepRow("SVC coef0=1e300",   mk, SVC_OFF.coef0, "f64", 1e300,    "accept");
}
{
    const mk = () => new ml.GaussianMixture(2, { seed: 1 }).fit(X);
    sweepRow("GaussianMixture maxIter=10001", mk, GMM_OFF.maxIter, "u64", 10001, "refuse");
    sweepRow("GaussianMixture maxIter=0",     mk, GMM_OFF.maxIter, "u64", 0,     "accept");
    sweepRow("GaussianMixture tol=NaN",       mk, GMM_OFF.tol, "f64", NaN,        "accept");
    sweepRow("GaussianMixture reg=NaN",       mk, GMM_OFF.reg, "f64", NaN,        "accept");
}
{
    const mk = () => new ml.LogisticRegression({ maxIter: 50 }).fit(X, yc);
    sweepRow("LogisticRegression maxIter=100001", mk, LOGREG_OFF.maxIter, "u64", 100001, "refuse");
    sweepRow("LogisticRegression maxIter=0",     mk, LOGREG_OFF.maxIter, "u64", 0,     "accept");
    sweepRow("LogisticRegression lr=NaN", mk, LOGREG_OFF.lr, "f64", NaN,      "accept");
    sweepRow("LogisticRegression lr=0",   mk, LOGREG_OFF.lr, "f64", 0,        "accept");
    sweepRow("LogisticRegression tol=NaN", mk, LOGREG_OFF.tol, "f64", NaN,    "accept");
    sweepRow("LogisticRegression l1=NaN",  mk, LOGREG_OFF.l1, "f64", NaN,     "accept");
}
{
    const mk = () => new ml.DecisionTreeClassifier({ seed: 5, maxDepth: 3 }).fit(X, yc);
    sweepRow("DecisionTreeClassifier maxDepth=2^62", mk, FOREST_OFF.maxDepth, "u64", 4611686018427387904, "accept-capped");
    sweepRow("DecisionTreeClassifier minSamplesSplit=0", mk, FOREST_OFF.minSamplesSplit, "u64", 0, "accept");
    sweepRow("DecisionTreeClassifier minSamplesLeaf=0",  mk, FOREST_OFF.minSamplesLeaf,  "u64", 0, "accept");
    sweepRow("DecisionTreeClassifier maxBins=2^62",      mk, FOREST_OFF.maxBins, "u64", 4611686018427387904, "accept");
    sweepRow("DecisionTreeClassifier maxFeatures=2^62",  mk, FOREST_OFF.maxFeatures, "u64", 4611686018427387904, "accept");
}

print("");
print("== 4. a refused record leaves no usable model behind ==");
{
    const base = new ml.DBScan(0.3, 2).fit(X);
    const rec = base.serialize();
    base.close();
    for (const [label, v] of [["0", 0], ["NaN", NaN], ["-Inf", -Infinity]]) {
        let threw = false, msg = "";
        try { const m = ml.DBScan.deserialize(patch(rec, 0, "f64", v)); m.close(); }
        catch (e) { threw = true; msg = String(e.message); }
        ok(threw, "DBScan with a forged eps=" + label + " throws rather than half-loading");
        ok(/DBScan record|expected|malformed|not a/.test(msg),
           "DBScan eps=" + label + " refusal names the record", msg.slice(0, 60));
    }
    const again = patch(rec, 0, "f64", 0);
    let threw = false;
    try { ml.DBScan.deserialize(again).close(); } catch (e) { threw = true; }
    ok(threw, "the refusal is repeatable");
}

print("");
print("test_ml_deser_validate: " + (fail ? "FAILURES: " + fail + " / " : "all ") +
      (pass + fail) + " assertions passed");
if (fail)
    throw new Error("test_ml_deser_validate: " + fail + " of " + (pass + fail) + " assertions FAILED");
