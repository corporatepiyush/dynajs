// covers: dyna:encoding.hexDecodeInto
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_encoding_hex_decode_into_encode_into");
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

__kitS.section("corpus is >= 100 vectors", () => {
    const vecs = corpus();
    ok(vecs.length >= 100, "corpus is >= 100 vectors (got " + vecs.length + ")");
    for (const [name, oneShot, into, bound] of CODECS) {
        const exact = name === "hex" || name === "base64" ||
                      name === "base32" || name === "base32hex";
        for (const v of vecs) {
            const want = oneShot(v);
            const out = new Uint8Array(bound(v.length) + 8).fill(0xa5);
            const count = into(v, out);
            eq(count, want.length, name + " count for len " + v.length);
            eq(dec.decode(out.subarray(0, count)), want,
               name + " bytes for len " + v.length);
            if (exact)
                eq(out[count], 0xa5, name + " nothing written past the count");
            ok(out.subarray(bound(v.length)).every((b) => b === 0xa5),
               name + " scratch never escapes the documented bound");
            const tight = new Uint8Array(bound(v.length));
            const c2 = into(v, tight);
            eq(c2, count, name + " exact-size count for len " + v.length);
            eq(dec.decode(tight.subarray(0, c2)), want,
               name + " exact-size bytes for len " + v.length);
            if (name === "hex")
                eq(hexDecodeInto(want, new Uint8Array(v.length)), v.length,
                   name + " decodeInto accepts its own encodeInto text");
        }
    }
});
__kitS.end();

print("test_encoding_encode_into: " + n + " assertions, " + bad + " failures");
if (bad) throw new Error(bad + " failures");
