// covers: dyna:encoding.hexEncodeInto
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_encoding_hex_encode_into");
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

__kitS.section("encodeInto: output needs at least", () => {
    for (const [name, oneShot, into, bound] of CODECS) {
        const v = rndBytes(20);
        const need = bound(20);
        if (need === 0) continue;
        const out = new Uint8Array(need - 1).fill(0xa5);
        throws(() => into(v, out), RangeError,
               new RegExp("encodeInto: output needs at least " + need),
               name + " short buffer refuses");
        ok(out.every((b) => b === 0xa5),
           name + " short buffer writes NOTHING (sentinel intact)");
    }
    eq(hexEncodeInto(new Uint8Array(0), new Uint8Array(0)), 0,
       "empty input writes 0 bytes into an empty buffer");
    eq(base58EncodeInto(new Uint8Array(0), new Uint8Array(0)), 0,
       "empty base58 input writes 0 bytes into an empty buffer");
});
__kitS.end();

{
    throws(() => hexEncodeInto(42, new Uint8Array(8)), TypeError, /./,
           "a number is not a byte input");
    eq(hexEncodeInto("abc", new Uint8Array(8)), 6,
       "a string input is its UTF-8 bytes (parity with HexEncode)");
    eq(dec.decode((() => { const o = new Uint8Array(8);
        const c = hexEncodeInto("abc", o); return o.subarray(0, c); })()),
       HexEncode("abc"), "string input byte-identity");
    throws(() => hexEncodeInto(new Uint8Array([1]), {}), TypeError, /./,
           "a plain object is not a byte view");
    throws(() => hexEncodeInto(), TypeError, /./, "missing arguments refuse");
    {
        const io = new Uint8Array(32);
        throws(() => hexEncodeInto(io.subarray(0, 8), io), TypeError, /overlap/,
               "in-place hexEncodeInto refuses");
        const io2 = new Uint8Array(64);
        throws(() => hexEncodeInto(io2.subarray(0, 32), io2), TypeError, /overlap/,
               "exact-capacity in-place hexEncodeInto refuses");
        throws(() => base58EncodeInto(io.subarray(0, 16), io), TypeError,
               /overlap/, "overlapping views refuse (base58)");
        throws(() => base64EncodeInto(io.subarray(0, 4), io.subarray(2, 30)),
               TypeError, /overlap/, "offset-overlapping views refuse");
        eq(hexEncodeInto(io.subarray(0, 4), io.subarray(16, 24)), 8,
           "disjoint views in one buffer are fine");
    }
    const ab = new ArrayBuffer(8);
    eq(hexEncodeInto(new Uint8Array([0xde, 0xad]), ab), 4, "ArrayBuffer out");
    eq(dec.decode(new Uint8Array(ab).subarray(0, 4)), "dead", "wrote into it");
    const big = new Uint8Array(64).fill(0xa5);
    const view = big.subarray(16, 24);
    const c = hexEncodeInto(new Uint8Array([0xbe, 0xef]), view);
    eq(c, 4, "subarray view accepted");
    eq(dec.decode(big.subarray(16, 20)), "beef", "written at the view offset 0");
    eq(big[15], 0xa5, "neighbors untouched");
    eq(big[20], 0xa5, "neighbors untouched after");
}

print("test_encoding_encode_into: " + n + " assertions, " + bad + " failures");
if (bad) throw new Error(bad + " failures");
