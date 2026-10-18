// flags: --std
// Split from tests/test_audit_w4_native_1.js: the encoding sections.
import "../lib/httpc.js";
import * as bytes from "dyna:bytes";
import * as enc from "dyna:encoding";
import * as ser from "dyna:serialize";
import * as crypto from "dyna:crypto";
import { suite } from "../lib/kit.js";

const t = suite("test_encoding_audit_w4_native_1");
const per = (f, n = 20000) => { let b = Infinity; for (let r = 0; r < 5; r++) { const t0 = performance.now(); for (let i = 0; i < n; i++) f(); b = Math.min(b, (performance.now() - t0) / n * 1e6); } return b; };
// ORACLE: what a byte-taking function returns is a property of the BYTES, not
// of the container they arrive in: a Uint8Array, a DataView and an ArrayBuffer
// over the same 16 bytes give the same answer. The published SHA-256 of sixteen
// 'A' bytes (FIPS 180-4, computed with OpenSSL) anchors one of them.
// SCALING: so the container cannot change the COST either. It did: every
// helper tried one container first by calling an accessor that throws on the
// other, then discarded the exception -- about 1.8 us per call, 15x the work.
// CONTROL: a string input, which never went through that probe.
t.test("byte containers agree and cost the same", ({ ok, eq }) => {
    const u8 = new Uint8Array(16).fill(65), ab = u8.buffer.slice(0), dv = new DataView(u8.buffer.slice(0));
    const td = new TextDecoder();
    const fns = {
        "TextDecoder.decode": (x) => td.decode(x),
        "HexEncode": (x) => enc.HexEncode(x),
        "Base64Encode": (x) => enc.Base64Encode(x),
        "SHA256Hex": (x) => crypto.SHA256Hex(x),
        "bytes.isValidUtf8": (x) => bytes.isValidUtf8(x),
        "bytes.equal": (x) => bytes.equal(x, x),
    };
    eq(crypto.SHA256Hex(u8), "991204fba2b6216d476282d375ab88d20e6108d109aecded97ef424ddd114706", "SHA-256 of sixteen 'A' bytes");
    for (const [name, f] of Object.entries(fns)) {
        const a = f(u8), b = f(ab), c = f(dv);
        ok(a === b && b === c, name + ": Uint8Array, ArrayBuffer and DataView give one answer (" + a + " / " + b + " / " + c + ")");
        const cu = per(() => f(u8)), cb = per(() => f(ab)), cd = per(() => f(dv));
        const lo = Math.min(cu, cb, cd), hi = Math.max(cu, cb, cd);
        ok(hi < lo * 4 + 150, "[red] " + name + ": no container costs several times another (" + cu.toFixed(0) + " / " + cb.toFixed(0) + " / " + cd.toFixed(0) + " ns)");
    }
    eq(enc.HexEncode("AAAAAAAAAAAAAAAA"), enc.HexEncode(u8), "control: the string form agrees");
});
// ORACLE: CBOR (RFC 8949 3.1) and MessagePack encode a map as a header plus
// its pairs; encoding N small maps is N times a constant, and a JSON
// serialisation of the same value does the same walk. So binary encoding
// cannot cost several times JSON.stringify -- it cost 15x, because every plain
// object was first probed as a byte view by an accessor that throws.
// RFC 8949 Appendix A gives the vectors: {"a": 1, "b": [2, 3]} is
// a26161016162820203, [1, [2, 3], [4, 5]] is 8301820203820405.
// CONTROL: a flat array of numbers, which never took the probe.
t.test("encoding plain objects costs what the walk costs", ({ ok, eq, deep, best }) => {
    const hex = (u) => enc.HexEncode(u);
    eq(hex(ser.CBOREncode({ a: 1, b: [2, 3] })), "a26161016162820203", "RFC 8949 Appendix A map");
    eq(hex(ser.CBOREncode([1, [2, 3], [4, 5]])), "8301820203820405", "RFC 8949 Appendix A nested array");
    const objs = new Array(20000).fill(0).map((_, i) => ({ a: i, s: "v" + i }));
    deep(ser.CBORDecode(ser.CBOREncode(objs.slice(0, 50))), objs.slice(0, 50), "CBOR round trip");
    deep(ser.MsgPackDecode(ser.MsgPackEncode(objs.slice(0, 50))), objs.slice(0, 50), "MessagePack round trip");
    deep(ser.structuredClone(objs.slice(0, 50)), objs.slice(0, 50), "structuredClone copies the values");
    const tj = best(() => JSON.stringify(objs)), tc = best(() => ser.CBOREncode(objs)), tm = best(() => ser.MsgPackEncode(objs)), ts = best(() => ser.structuredClone(objs));
    ok(tc < tj * 5, "[red] CBOREncode of 20000 small objects costs about what JSON.stringify does (" + tc.toFixed(2) + " ms vs " + tj.toFixed(2) + " ms)");
    ok(tm < tj * 5, "[red] MsgPackEncode likewise (" + tm.toFixed(2) + " ms)");
    ok(ts < tj * 12, "[red] structuredClone builds a copy in a small multiple of that (" + ts.toFixed(2) + " ms)");
    const flat = new Array(100000).fill(0).map((_, i) => i * 1.5);
    ok(best(() => ser.CBOREncode(flat)) < best(() => JSON.stringify(flat)) * 3, "control: a flat number array was never slow");
});
await t.run();
