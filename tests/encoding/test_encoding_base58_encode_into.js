// covers: dyna:encoding.base58EncodeInto
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_encoding_base58_encode_into");
import {
    HexEncode, hexEncodeInto, hexDecodeInto,
    Base64Encode, base64EncodeInto,
    Base64URLEncode, base64UrlEncodeInto,
    Base32Encode, base32EncodeInto,
    Base32HexEncode, base32HexEncodeInto,
    Base85Encode, base85EncodeInto,
    Base58Encode, base58EncodeInto, base58DecodeInto,
    Base58CheckEncode, base58CheckEncodeInto, base58CheckDecodeInto,
} from "dyna:encoding";
import { SHA256 } from "dyna:crypto";

let n = 0, bad = 0;
function ok(c, what) { n++; if (!c) { bad++; print("FAIL: " + what); } }
function eq(a, b, what) { ok(a === b, what + " (got " + a + ", want " + b + ")"); }
function throws(fn, ErrType, re, what) {
    let t = null;
    try { fn(); } catch (e) { t = e; }
    ok(t !== null, "expected throw: " + what);
    if (t === null) return;
    ok(t instanceof ErrType,
       what + " (wrong type: " + (t && t.constructor && t.constructor.name) + ")");
    ok(!re || re.test(String(t.message)),
       what + " (message [" + String(t && t.message) + "] !~ " + re + ")");
}
const dec = new TextDecoder();

let seed = 0x9e3779b9;
function rnd() {
    seed ^= seed << 13; seed >>>= 0;
    seed ^= seed >>> 17;
    seed ^= seed << 5;  seed >>>= 0;
    return seed;
}
function rndBytes(len) {
    const u = new Uint8Array(len);
    for (let i = 0; i < len; i++) u[i] = rnd() & 0xff;
    return u;
}

const CODECS = [
    ["hex",      HexEncode,      hexEncodeInto,      (n) => 2 * n],
    ["base64",   Base64Encode,   base64EncodeInto,   (n) => 4 * Math.ceil(n / 3)],
    ["base64url",Base64URLEncode,base64UrlEncodeInto,(n) => 4 * Math.ceil(n / 3)],
    ["base32",   Base32Encode,   base32EncodeInto,   (n) => Math.ceil(n / 5) * 8],
    ["base32hex",Base32HexEncode,base32HexEncodeInto,(n) => Math.ceil(n / 5) * 8],
    ["base85",   Base85Encode,   base85EncodeInto,   (n) => Math.ceil(n / 4) * 5],
    ["base58",   Base58Encode,   base58EncodeInto,   (n) => n ? Math.floor(n * 8 / 5) + 1 : 0],
    ["base58check",Base58CheckEncode,base58CheckEncodeInto,
                              (n) => Math.floor((n + 4) * 8 / 5) + 1],
];

function corpus() {
    const list = [
        new Uint8Array(0),
        new Uint8Array([0]), new Uint8Array([1]), new Uint8Array([255]),
        new Uint8Array([0, 0]), new Uint8Array([0, 0, 0, 0]),
        new Uint8Array([0, 1]), new Uint8Array([1, 0]),
        new Uint8Array([0, 0, 255, 255]),
        new Uint8Array(9).fill(255),
    ];
    for (let len = 1; len <= 9; len++) {
        list.push(rndBytes(len));
        list.push(new Uint8Array(len).fill(0));
        list.push(new Uint8Array(len).fill(0xff));
    }
    while (list.length < 110) list.push(rndBytes(1 + (rnd() % 64)));
    return list;
}

__kitS.section("leading zeros length", () => {
    eq(base58EncodeInto(new Uint8Array([0, 0, 1]), new Uint8Array(8)),
       Base58Encode(new Uint8Array([0, 0, 1])).length,
       "leading zeros length");
    eq(dec.decode((() => { const o = new Uint8Array(8);
        const c = base58EncodeInto(new Uint8Array([0, 0, 1]), o);
        return o.subarray(0, c); })()),
       "112", "two leading zero bytes are two '1's");
    {
        const v = rndBytes(30);
        const out = new Uint8Array(Math.floor(34 * 8 / 5) + 1 + 8);
        const c = base58CheckEncodeInto(v, out);
        eq(dec.decode(out.subarray(0, c)), Base58CheckEncode(v),
           "base58check byte-identity");
        const back = new Uint8Array(64);
        const r = base58CheckDecodeInto(dec.decode(out.subarray(0, c)), back);
        eq(r, v.length, "check round-trip length");
        ok(v.every((b, i) => back[i] === b), "check round-trip bytes");
    }
    {
        const v = rndBytes(4096);
        const out = new Uint8Array(Math.floor(4096 * 8 / 5) + 1 + 8);
        const c = base58EncodeInto(v, out);
        eq(dec.decode(out.subarray(0, c)), Base58Encode(v),
           "4096-byte input (the quadratic cap) is byte-exact");
        throws(() => base58EncodeInto(rndBytes(4097), new Uint8Array(8192)),
               RangeError, /exceeds 4096 bytes/, "4097 bytes refuses (cap)");
        const w = rndBytes(2000);
        const out2 = new Uint8Array(Math.floor(2000 * 8 / 5) + 1 + 8);
        const c2 = base58EncodeInto(w, out2);
        const back = new Uint8Array(4096);
        eq(base58DecodeInto(dec.decode(out2.subarray(0, c2)), back), 2000,
           "2000-byte round-trip");
        ok(w.every((b, i) => back[i] === b), "2000-byte round-trip bytes");
    }
});
__kitS.end();

print("test_encoding_encode_into: " + n + " assertions, " + bad + " failures");
if (bad) throw new Error(bad + " failures");
