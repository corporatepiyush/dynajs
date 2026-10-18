// flags: --std
// Split from tests/test_audit_w3_native_8.js: the ml sections.
import * as ml from "dyna:ml";
import { Path, makeTempDir, removeAll } from "dyna:file";

let pass = 0, fail = 0;
function ok(c, m) { if (c) pass++; else { fail++; console.log("FAIL: " + m); } }
function kind(f) { try { f(); return "none"; } catch (e) { return e.constructor.name; } }
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const T = makeTempDir("w3n8_");
const tp = (n) => String(T) + "/" + n;
// ---- M1b-07: persisted tree models --------------------------------------
// ORACLE: the constructor enforces subsample in (0, 1], so a record holding
// NaN there was not written by this library; and a record's family flags must
// agree with the class asked to load it, or X.deserialize returns an X that
// behaves as another family. The record is sealed with CRC-32C, so the test
// re-seals after each mutation: only the range check can refuse it.
// CONTROL: the untouched record loads and predicts as the original.
{
    const Tb = new Uint32Array(256);
    for (let i = 0; i < 256; i++) { let c = i; for (let k = 0; k < 8; k++) c = (c & 1) ? (c >>> 1) ^ 0x82F63B78 : c >>> 1; Tb[i] = c >>> 0; }
    const crc = (b, n) => { let c = 0xFFFFFFFF; for (let i = 0; i < n; i++) c = Tb[(c ^ b[i]) & 255] ^ (c >>> 8); return (c ^ 0xFFFFFFFF) >>> 0; };
    const reseal = (b) => new DataView(b.buffer, b.byteOffset).setUint32(b.length - 4, crc(b, b.length - 4), true);
    const X = [], y = [];
    for (let i = 0; i < 24; i++) { const row = [Math.sin(i * 1.3), Math.cos(i * 0.7), (i % 5) - 2]; X.push(row); y.push(row[0] * 2 - row[1]); }
    const m = await new ml.GradientBoostingRegressor({ nEstimators: 3, subsample: 0.75 }).fit(X, y);
    const rec = new Uint8Array(m.serialize());
    const back = ml.GradientBoostingRegressor.deserialize(rec);
    ok(JSON.stringify(Array.from(await back.predict(X))) === JSON.stringify(Array.from(await m.predict(X))), "control: an untouched record round-trips");

    const dv = new DataView(rec.buffer, rec.byteOffset, rec.byteLength);
    let at = -1;
    for (let i = 0; i + 8 <= rec.length - 4; i++) if (dv.getFloat64(i, true) === 0.75) { at = i; break; }
    ok(at > 0, "the test located the subsample field (offset " + at + ")");
    const bad = rec.slice();
    new DataView(bad.buffer).setFloat64(at, NaN, true);
    reseal(bad);
    ok(kind(() => ml.GradientBoostingRegressor.deserialize(bad)) === "TypeError", "[red] M1b-07: a record with subsample NaN is refused");
    const neg = rec.slice();
    new DataView(neg.buffer).setFloat64(at, 7.5, true);
    reseal(neg);
    ok(kind(() => ml.GradientBoostingRegressor.deserialize(neg)) === "TypeError", "[red] M1b-07: a record with subsample 7.5 is refused");
}
removeAll(new Path(String(T)));
console.log("test_ml_audit_w3_native_8: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("test_ml_audit_w3_native_8: " + fail + " failures");
