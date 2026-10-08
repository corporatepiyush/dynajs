// timeout: 600
import * as ml from "dyna:ml";

let pass = 0, fail = 0;
const ok = (c, w, d) => { if (c) pass++; else { fail++; print("  FAIL  " + w + (d ? "  [" + d + "]" : "")); } };

const HDR = 20;
const TBL = (() => { const t = new Uint32Array(256);
    for (let n = 0; n < 256; n++) { let c = n; for (let k = 0; k < 8; k++) c = (c & 1) ? (0x82F63B78 ^ (c >>> 1)) : (c >>> 1); t[n] = c >>> 0; }
    return t; })();
function crc32c(b, n) { let c = 0xFFFFFFFF; for (let i = 0; i < n; i++) c = TBL[(c ^ b[i]) & 0xFF] ^ (c >>> 8); return (c ^ 0xFFFFFFFF) >>> 0; }

function patch(rec, off, kind, v, crc) {
    const r = rec.slice();
    const dv = new DataView(r.buffer, r.byteOffset, r.byteLength);
    const o = HDR + off;
    if (kind === "f64") dv.setFloat64(o, v, true);
    else if (kind === "u64") dv.setBigUint64(o, BigInt(v), true);
    else dv.setUint32(o, v >>> 0, true);
    if (crc !== false) dv.setUint32(r.length - 4, crc32c(r, r.length - 4), true);
    return r;
}

const F = { classifier: 0, boosting: 4, n_trees: 8, n_rounds: 16, n_features: 24,
            n_classes: 32, has_classes: 40, n_out: 44 };
const D = { eps: 0, minPts: 8, rows: 16, nClusters: 24 };

const N = 200, C = 2;
const X = new Float64Array(N * C), y = new Float64Array(N);
for (let i = 0; i < N; i++) {
    X[i * C] = (i % 5) * 1.0; X[i * C + 1] = ((i % 5) * 1.0 + (i % 3) * 0.1);
    y[i] = i % 2;
}
const yc = Array.from(y);

print("== 1. a forged n_features is refused, CRC-valid, across the classes ==");
{
    const CLASSES = ["DecisionTreeClassifier", "DecisionTreeRegressor",
                     "RandomForestClassifier", "RandomForestRegressor",
                     "GradientBoostingClassifier", "GradientBoostingRegressor",
                     "XGBClassifier", "XGBRegressor"];
    for (const name of CLASSES) {
        const Ctor = ml[name];
        const opts = name === "DecisionTreeClassifier" || name === "DecisionTreeRegressor"
            ? { seed: 1 } : { seed: 1, nEstimators: 3 };
        const m = new Ctor(opts);
        let rec, cap;
        try {
            m.fit(X, y, N, C);
            rec = m.serialize();
            cap = Math.floor((rec.length - HDR - 4 - 8) / 8);
        } finally { m.close(); }
        ok(cap > C, name + ": the record can honestly claim more than " + C + " columns",
           "cap=" + cap);
        const VALUES = [["0", 0],
                        ["cap+1 (" + (cap + 1) + ")", cap + 1],
                        ["4*cap+1", 4 * cap + 1],
                        ["10^6", 1000000],
                        ["2^40", 1099511627776],
                        ["2^63", 9223372036854775808],
                        ["2^64-1", 18446744073709551615]];
        let loaded = 0;
        for (const [label, v] of VALUES) {
            const forged = patch(rec, F.n_features, "u64", v);
            let g = null, msg = "";
            const t0 = Date.now();
            try { g = Ctor.deserialize(forged); } catch (e) { msg = String(e.message); }
            const dt = Date.now() - t0;
            ok(g === null, name + ": forged n_features=" + label + " is refused",
               g === null ? "" : "LOADED");
            if (g) {
                loaded++;
                if (v > 0 && v <= 1000000) {
                    let impLen = -1;
                    try { impLen = g.featureImportances.length; } catch (e) { impLen = -1; }
                    ok(impLen === v, name + ": a loaded n_features=" + label +
                       " does not hand featureImportances a " + v + "-entry array", String(impLen));
                }
                try { g.close(); } catch (e) {  }
            }
            ok(dt < 2000, name + ": refusing n_features=" + label + " is prompt", dt + " ms" + (msg ? " (" + msg.slice(0, 40) + ")" : ""));
        }
        ok(loaded === 0, name + ": no forged n_features loaded at all", loaded + " loaded");
        let lo = 1, hi = cap, best = 0;
        while (lo <= hi) {
            const mid = (lo + hi) >> 1;
            const g = (() => { try { return Ctor.deserialize(patch(rec, F.n_features, "u64", mid)); }
                               catch (e) { return null; } })();
            if (g) { best = mid; lo = mid + 1; try { g.close(); } catch (e) {  } }
            else hi = mid - 1;
        }
        ok(best > C, name + ": the gate is a payload bound, not a clamp (accepts up to " + best + ")",
           "only " + best);
        ok(best <= cap, name + ": the gate is never looser than the payload (" + best + " <= " + cap + ")",
           "accepted " + best);
        const overBy1 = (() => { try { return Ctor.deserialize(patch(rec, F.n_features, "u64", best + 1)); }
                                 catch (e) { return null; } })();
        ok(overBy1 === null, name + ": n_features=" + (best + 1) + " is one past the bound and is refused",
           overBy1 === null ? "" : "LOADED");
        if (overBy1) try { overBy1.close(); } catch (e) {  }
    }
}

print("");
print("== 2. the fix does not refuse a record the module can legitimately write ==");
{
    for (const name of ["DecisionTreeClassifier", "RandomForestClassifier",
                        "XGBRegressor", "GradientBoostingRegressor"]) {
        const Ctor = ml[name];
        const m = new Ctor({ seed: 1, nEstimators: 3 });
        let rec, g, nfeat;
        try { m.fit(X, y, N, C); rec = m.serialize(); nfeat = C; } finally { m.close(); }
        try {
            g = Ctor.deserialize(rec);
            ok(g !== null, name + ": an untampered record still loads");
            if (g) {
                const imp = g.featureImportances;
                ok(imp && imp.length === C, name + ": featureImportances is still " + C + " long",
                   imp ? String(imp.length) : "null");
                try { g.close(); } catch (e) {  }
            }
        } catch (e) { ok(false, name + ": an untampered record still loads", String(e.message).slice(0, 60)); }
    }
}

print("");
print("== 3. the same gate on every OTHER codec, pinned row by row ==");
{
    const lr = new ml.LinearRegression();
    let lrRec;
    try { lr.fit(X, y, N, C); lrRec = lr.serialize(); } finally { lr.close(); }
    for (const [label, v] of [["0", 0], ["10^6", 1000000], ["2^64-1", 18446744073709551615]]) {
        const g = (() => { try { return ml.LinearRegression.deserialize(patch(lrRec, 0, "u64", v)); }
                            catch (e) { return null; } })();
        ok(g === null, "LinearRegression: forged n_features=" + label + " is refused",
           g === null ? "" : "LOADED");
        if (g) try { g.close(); } catch (e) {  }
    }

    const sc = new ml.StandardScaler();
    let scRec;
    try { sc.fit(X, N, C); scRec = sc.serialize(); } finally { sc.close(); }
    for (const [label, v] of [["0", 0], ["10^6", 1000000], ["2^64-1", 18446744073709551615]]) {
        const g = (() => { try { return ml.StandardScaler.deserialize(patch(scRec, 4, "u64", v)); }
                            catch (e) { return null; } })();
        ok(g === null, "StandardScaler: forged n_features=" + label + " is refused",
           g === null ? "" : "LOADED");
        if (g) try { g.close(); } catch (e) {  }
    }

    const km = new ml.KMeans(3, { seed: 7 });
    let kmRec;
    try { km.fit(X, N, C); kmRec = km.serialize(); } finally { km.close(); }
    for (const [off, field, v, label] of [[0, "k", 0, "0"], [0, "k", 2 ** 40, "2^40"],
                                          [8, "n_features", 0, "0"],
                                          [8, "n_features", 2 ** 40, "2^40"]]) {
        const g = (() => { try { return ml.KMeans.deserialize(patch(kmRec, off, "u64", v)); }
                            catch (e) { return null; } })();
        ok(g === null, "KMeans: forged " + field + "=" + label + " is refused",
           g === null ? "" : "LOADED");
        if (g) try { g.close(); } catch (e) {  }
    }

    const kn = new ml.KNClassifier(3);
    let knRec;
    try { kn.fit(X, yc, N, C); knRec = kn.serialize(); } finally { kn.close(); }
    for (const [off, field, v, label] of [[8, "k", 0, "0"], [8, "k", 2 ** 40, "2^40"],
                                          [16, "rows", 2 ** 40, "2^40"],
                                          [24, "cols", 2 ** 40, "2^40"]]) {
        const g = (() => { try { return ml.KNClassifier.deserialize(patch(knRec, off, "u64", v)); }
                            catch (e) { return null; } })();
        ok(g === null, "KNClassifier: forged " + field + "=" + label + " is refused",
           g === null ? "" : "LOADED");
        if (g) try { g.close(); } catch (e) {  }
    }
}

print("");
print("== 4. DBScan's own restored fields ==");
{
    const base = new ml.DBScan(0.3, 2);
    let rec;
    try { base.fit([[0, 0], [0.1, 0.2], [1, 1], [1.1, 0.9], [2, 2], [2.2, 1.8]]); rec = base.serialize(); }
    finally { base.close(); }
    for (const [off, kind, field, label] of [[D.minPts, "u64", "minPts", "0"],
                                              [D.minPts, "u64", "minPts", "2^64-1"],
                                              [D.rows, "u64", "rows", "2^40"]]) {
        const v = label === "0" ? 0 : (label === "2^40" ? 1099511627776 : 18446744073709551615);
        const g = (() => { try { return ml.DBScan.deserialize(patch(rec, off, kind, v)); }
                            catch (e) { return null; } })();
        ok(g === null, "DBScan: forged " + field + "=" + label + " is refused",
           g === null ? "" : "LOADED");
        if (g) try { g.close(); } catch (e) {  }
    }
    const g = ml.DBScan.deserialize(rec);
    try {
        ok(g !== null, "an untampered DBScan record still loads");
        ok(g.labels.length === 6, "and still has 6 labels", g ? String(g.labels.length) : "n/a");
    } finally { if (g) g.close(); }
}

print("");
print("== 5. control: a byte flip with NO CRC fix is refused by the envelope ==");
{
    const base = new ml.DBScan(0.3, 2);
    let rec;
    try { base.fit([[0, 0], [0.1, 0.2], [1, 1], [1.1, 0.9]]); rec = base.serialize(); } finally { base.close(); }
    const flipped = patch(rec, D.minPts, "u64", 0, false);
    let threw = false;
    try { ml.DBScan.deserialize(flipped).close(); } catch (e) { threw = true; }
    ok(threw, "an un-CRCed byte flip is refused as corrupt");
    const resealed = patch(rec, D.minPts, "u64", 0, true);
    let threw2 = false;
    try { ml.DBScan.deserialize(resealed).close(); } catch (e) { threw2 = true; }
    ok(threw2, "the same flip WITH a recomputed CRC is refused by the validator, not the CRC");
}

print("");
print("test_ml_dbscan_forge: " + (fail ? "FAILURES: " + fail + " / " : "all ") +
      (pass + fail) + " assertions passed");
if (fail)
    throw new Error("test_ml_dbscan_forge: " + fail + " of " + (pass + fail) + " assertions FAILED");
