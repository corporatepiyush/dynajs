// covers: dyna:encoding.Base58CheckEncode
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_encoding_base58_check_encode_into");
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

const B58_ALPHA = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

function b58Ref(bytes) {
    let zeros = 0;
    while (zeros < bytes.length && bytes[zeros] === 0) zeros++;
    let v = 0n;
    for (let i = zeros; i < bytes.length; i++) v = v * 256n + BigInt(bytes[i]);
    let out = "";
    while (v > 0n) { out = B58_ALPHA[Number(v % 58n)] + out; v /= 58n; }
    return "1".repeat(zeros) + out;
}

function b58CheckRef(bytes) {
    const h1 = SHA256(bytes), h2 = SHA256(h1);
    const payload = new Uint8Array(bytes.length + 4);
    payload.set(bytes, 0);
    payload.set(h2.subarray(0, 4), bytes.length);
    return b58Ref(payload);
}

__kitS.section("all-zero", () => {
    const encCheck = (data) => {
        const out = new Uint8Array(Math.floor((data.length + 4) * 8 / 5) + 8);
        const c = base58CheckEncodeInto(data, out);
        return dec.decode(out.subarray(0, c));
    };
    for (const z of [193, 1337, 1880, 2472]) {
        const data = new Uint8Array(z);
        const want = Base58CheckEncode(data);
        const got = encCheck(data);
        eq(got, want, "all-zero " + z + ": Into == one-shot");
        eq(want, b58CheckRef(data), "all-zero " + z + ": one-shot == BigInt reference");
        const txt = got;
        const buf = new Uint8Array(txt.length);
        eq(base58CheckDecodeInto(txt, buf), z,
           "all-zero " + z + ": the Into output decodes back to " + z + " bytes");
        ok(buf.subarray(0, z).every((b) => b === 0),
           "all-zero " + z + ": the decoded bytes are zero");
    }
    eq(encCheck(new Uint8Array(0)), Base58CheckEncode(new Uint8Array(0)),
       "empty input: Into == one-shot");
    eq(encCheck(new Uint8Array(0)), b58CheckRef(new Uint8Array(0)),
       "empty input: Into == BigInt reference");
    let mism = 0;
    for (let z = 0; z <= 4096; z++) {
        const data = new Uint8Array(z);
        const want = Base58CheckEncode(data);
        if (encCheck(data) !== want || want !== b58CheckRef(data)) {
            mism++;
            if (mism < 4) print("FAIL: all-zero base58check len " + z);
        }
    }
    eq(mism, 0, "all-zero sweep 0..4096: Into == one-shot == BigInt reference");
    let fmis = 0;
    for (let t = 0; t < 300; t++) {
        const len = 1 + (rnd() % 120);
        const v = rndBytes(len);
        if (t % 3 === 0) v.fill(0, 0, Math.min(len, t % 17));
        if (t % 7 === 0) v[len - 1] = 0;
        const wantC = b58CheckRef(v);
        const outC = new Uint8Array(Math.floor((len + 4) * 8 / 5) + 8);
        const cC = base58CheckEncodeInto(v, outC);
        const wantP = b58Ref(v);
        const outP = new Uint8Array(Math.floor(len * 8 / 5) + 8);
        const cP = base58EncodeInto(v, outP);
        if (dec.decode(outC.subarray(0, cC)) !== wantC ||
            dec.decode(outP.subarray(0, cP)) !== wantP) {
            fmis++;
            if (fmis < 4) print("FAIL: base58 BigInt differential at len " + len);
        }
    }
    eq(fmis, 0,
       "300 random+adversarial inputs: both base58 forms == BigInt reference");
});
__kitS.end();

print("test_encoding_encode_into: " + n + " assertions, " + bad + " failures");
if (bad) throw new Error(bad + " failures");
