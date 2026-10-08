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
const X = [[0, 0], [0.1, 0.2], [1, 1], [1.1, 0.9], [2, 2], [2.2, 1.8], [0.5, 1.5], [1.5, 0.5]];
const y = [0.0, 0.2, 1.0, 0.9, 2.0, 1.8, 1.5, 0.5];

const which = scriptArgs[0] || "pca";
if (which === "pca") {
    const p = new ml.PCA();
    p.fit(X);
    const rec = p.serialize();
    p.close();
    const m = ml.PCA.deserialize(patch(rec, 12, "u64", 0));
    print("loaded zero-component PCA");
    m.transform([[1.0, 1.0]]);
    print("transform returned");
} else {
    const t = new ml.DecisionTreeRegressor({ seed: 3, maxDepth: 4 }).fit(X, y);
    const rec = t.serialize();
    t.close();
    const m = ml.DecisionTreeRegressor.deserialize(patch(rec, 148, "u32", 1));
    print("loaded newton=1 non-boosting forest");
    m.fit(X, y);
    print("refit returned");
}
