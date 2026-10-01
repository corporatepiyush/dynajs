// flags: --std
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

{
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
}

{
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
}

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

{
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
}

{
    for (let i = 0; i < 300; i++) {
        const v = rndBytes(rnd() % 201);
        for (const [name, oneShot, into, bound] of CODECS) {
            const want = oneShot(v);
            const out = new Uint8Array(bound(v.length));
            const c = into(v, out);
            if (c !== want.length || dec.decode(out.subarray(0, c)) !== want) {
                bad++;
                print("FAIL: scale differential " + name + " at len " + v.length);
                break;
            }
        }
    }
    n++;
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
{
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
}

print("test_encoding_encode_into: " + n + " assertions, " + bad + " failures");
if (bad) throw new Error(bad + " failures");
