// timeout: 600
import { DecisionTreeClassifier, DecisionTreeRegressor,
         RandomForestClassifier, RandomForestRegressor,
         GradientBoostingClassifier, GradientBoostingRegressor,
         XGBClassifier, XGBRegressor } from "dyna:ml";

let pass = 0, fail = 0;
const ok = (c, w, d) => { if (c) pass++; else { fail++; print("  FAIL  " + w + (d ? "  [" + d + "]" : "")); } };

const CAP = 1024;

const CLASSES = [
    ["DecisionTreeClassifier",      e => new DecisionTreeClassifier(Object.assign({ seed: 1 }, e))],
    ["DecisionTreeRegressor",       e => new DecisionTreeRegressor(Object.assign({ seed: 1 }, e))],
    ["RandomForestClassifier",      e => new RandomForestClassifier(Object.assign({ nEstimators: 2, seed: 1 }, e))],
    ["RandomForestRegressor",       e => new RandomForestRegressor(Object.assign({ nEstimators: 2, seed: 1 }, e))],
    ["GradientBoostingClassifier",  e => new GradientBoostingClassifier(Object.assign({ nEstimators: 2, seed: 1 }, e))],
    ["GradientBoostingRegressor",   e => new GradientBoostingRegressor(Object.assign({ nEstimators: 2, seed: 1 }, e))],
    ["XGBClassifier",               e => new XGBClassifier(Object.assign({ nEstimators: 2, seed: 1 }, e))],
    ["XGBRegressor",                e => new XGBRegressor(Object.assign({ nEstimators: 2, seed: 1 }, e))],
];
const isReg = name => /Regressor/.test(name);

function ladder(n) {
    const X = new Float64Array(n), y = new Float64Array(n);
    for (let i = 0; i < n; i++) { X[i] = i; y[i] = i % 2; }
    return [X, y];
}
const SMALL_N = 4000;
const [X, y] = ladder(SMALL_N);
const yr = new Float64Array(SMALL_N);
for (let i = 0; i < SMALL_N; i++) yr[i] = y[i];

print("== 1. maxDepth out of range is REFUSED on all eight classes ==");
const OVER = [
    ["1025", 1025], ["1026", 1026], ["2048", 2048], ["65536", 65536],
    ["1e6", 1000000], ["1e9", 1000000000], ["2^40", 1099511627776],
    ["2^62", 4611686018427387904], ["Infinity", Infinity], ["NaN", NaN],
    ["-1", -1], ["-1e9", -1000000000],
];
for (const [name, make] of CLASSES) {
    for (const [label, v] of OVER) {
        let refused = false, msg = "";
        try { const m = make({ maxDepth: v }); m.close(); }
        catch (e) { refused = true; msg = String(e.message); }
        ok(refused, name + " refuses maxDepth=" + label, "accepted");
        if (refused) ok(/maxDepth/.test(msg), name + " maxDepth=" + label + " names the option", msg.slice(0, 60));
    }
}

print("");
print("== 2. maxDepth in range is ACCEPTED on all eight classes, and obeyed ==");
const IN = [["1", 1], ["2", 2], ["3", 3], ["1023", 1023], ["1024", 1024]];
for (const [name, make] of CLASSES) {
    for (const [label, v] of IN) {
        let m = null, depth = -1, msg = "";
        try {
            m = make({ maxDepth: v });
            m.fit(X, isReg(name) ? yr : y, SMALL_N, 1);
            depth = m.depth;
        } catch (e) { msg = String(e.message); }
        if (m) m.close();
        ok(depth >= 0, name + " accepts maxDepth=" + label, msg.slice(0, 60));
        ok(depth <= Math.min(v, CAP), name + " maxDepth=" + label + " depth=" + depth + " <= " + Math.min(v, CAP));
    }
}

print("");
print("== 3. the cap holds with maxDepth absent, zero, and non-integer ==");
for (const [name, make] of CLASSES) {
    for (const [label, v, wantDepth] of [["absent", undefined, CAP], ["null", null, CAP],
                                         ["0", 0, CAP], ["2.9", 2.9, 2],
                                         ["1024.9", 1024.9, CAP]]) {
        let m = null, depth = -1, msg = "";
        try {
            m = make(v === undefined || v === null ? {} : { maxDepth: v });
            m.fit(X, isReg(name) ? yr : y, SMALL_N, 1);
            depth = m.depth;
        } catch (e) { msg = String(e.message); }
        if (m) m.close();
        ok(depth >= 0, name + " accepts maxDepth=" + label, msg.slice(0, 60));
        ok(depth <= wantDepth, name + " maxDepth=" + label + " depth=" + depth + " <= " + wantDepth);
    }
}

print("");
print("== 4. a FORGED record cannot raise the cap either ==");
{
    const HDR = 20;
    const T = (() => { const t = new Uint32Array(256);
        for (let n = 0; n < 256; n++) { let c = n; for (let k = 0; k < 8; k++) c = (c & 1) ? (0x82F63B78 ^ (c >>> 1)) : (c >>> 1); t[n] = c >>> 0; }
        return t; })();
    const crc32c = (b, n) => { let c = 0xFFFFFFFF; for (let i = 0; i < n; i++) c = T[(c ^ b[i]) & 0xFF] ^ (c >>> 8); return (c ^ 0xFFFFFFFF) >>> 0; };
    const MAXDEPTH_OFF = 108;
    function forge(rec, off, big) {
        const r = rec.slice();
        new DataView(r.buffer, r.byteOffset, r.byteLength).setBigUint64(HDR + off, big, true);
        new DataView(r.buffer, r.byteOffset, r.byteLength).setUint32(r.length - 4, crc32c(r, r.length - 4), true);
        return r;
    }
    const src = new DecisionTreeClassifier({ seed: 1, maxDepth: 4 })
        .fit(new Float64Array([0, 1, 2, 3]), new Float64Array([0, 1, 0, 1]), 4, 1);
    const rec = src.serialize();
    src.close();

    for (const [label, big] of [["1e9", BigInt("1000000000")], ["2^62", BigInt("4611686018427387904")],
                                ["2^64-1", 18446744073709551615n]]) {
        const r = forge(rec, MAXDEPTH_OFF, big);
        let m = null, depth = -1, msg = "";
        try {
            m = DecisionTreeClassifier.deserialize(r);
            m.fit(X, y, SMALL_N, 1);
            depth = m.depth;
        } catch (e) { msg = String(e.message); }
        if (m) m.close();
        ok(depth >= 0, "forged maxDepth=" + label + " record loads and refits", msg.slice(0, 60));
        ok(depth <= CAP, "forged maxDepth=" + label + " record still stops at " + CAP + ", got " + depth);
    }
    {
        const m = DecisionTreeClassifier.deserialize(rec);
        m.fit(X, y, SMALL_N, 1);
        ok(m.depth === 4, "the untampered record still refits to its own maxDepth=4, got " + m.depth);
        m.close();
    }
}

print("");
print("== 5. no input at all makes a tree crash the process ==");
for (const [name, make] of CLASSES) {
    let accepted = false;
    try { const m = make({ maxDepth: 1000000000 }); m.close(); accepted = true; }
    catch (e) { accepted = false; }
    if (!accepted) { pass++; continue; }
    const [XL, yL] = ladder(40000);
    let m = null, depth = -1;
    try {
        m = make({ maxDepth: 1000000000 });
        m.fit(XL, isReg(name) ? yr : yL, 40000, 1);
        depth = m.depth;
    } catch (e) {  }
    if (m) m.close();
    ok(depth > 0 && depth <= CAP, name + " survived a 40000-row ladder, depth=" + depth);
}

print("");
print("== 6. the other tree-family options are NOT unbounded either ==");
const OPTION_PROBES = [
    ["minSamplesSplit", 1,    "refused", "below its minimum of 2"],
    ["minSamplesSplit", 2,    "accept",  ""],
    ["minSamplesSplit", 1e18, "accept",  "a huge value just makes every node a leaf"],
    ["minSamplesLeaf",  1,    "accept",  ""],
    ["minSamplesLeaf",  4.6e18, "accept", "2 * it must not overflow size_t"],
    ["maxFeatures",     1e9,  "accept",  "clamped to the column count at use"],
    ["maxBins",         0,    "accept",  "0 is the exact splitter"],
    ["maxBins",         1,    "refused", "between 1 and DYN_HIST_MAX_BINS"],
    ["maxBins",         256,  "refused", "above DYN_HIST_MAX_BINS"],
    ["maxBins",         255,  "accept",  ""],
    ["nEstimators",     0,    "refused", "at least 1"],
    ["nEstimators",     100001, "refused", "above DYN_ML_MAX_TREES"],
    ["nEstimators",     100,  "accept",  ""],
    ["learningRate",    0,    "refused", "below 1e-12"],
    ["learningRate",    1e13, "refused", "above 1e12"],
    ["subsample",       0,    "refused", "below 1e-6"],
    ["subsample",       1.5,  "refused", "above 1.0"],
    ["seed",            -1,   "refused", "below 0"],
];
for (const [key, v, want, why] of OPTION_PROBES) {
    const Ctor = key === "nEstimators" ? RandomForestClassifier : DecisionTreeClassifier;
    let got = "accept", msg = "";
    try { const m = new Ctor({ seed: 1, [key]: v }); m.close(); }
    catch (e) { got = "refused"; msg = String(e.message); }
    ok(got === want, Ctor.name + " {" + key + ": " + v + "} -> " + want + " (" + why + ")",
       "got " + got + (msg ? ": " + msg.slice(0, 50) : ""));
}
{
    let m = null, depth = -1, msg = "";
    try {
        m = new DecisionTreeClassifier({ seed: 1, minSamplesLeaf: 4611686018427387904 });
        m.fit(X, y, SMALL_N, 1);
        depth = m.depth;
    } catch (e) { msg = String(e.message); }
    if (m) m.close();
    ok(depth >= 0, "a huge minSamplesLeaf fits without overflow", msg.slice(0, 60));
    ok(depth <= 1, "a huge minSamplesLeaf makes every node a leaf, depth=" + depth);
}

print("");
print("test_ml_depth_cap: " + (fail ? "FAILURES: " + fail + " / " : "all ") +
      (pass + fail) + " assertions passed");
if (fail)
    throw new Error("test_ml_depth_cap: " + fail + " of " + (pass + fail) + " assertions FAILED");
