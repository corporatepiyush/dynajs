// covers: dyna:encoding.uvarintAt
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_encoding_uvarint_at_into_at");
import {
    HexEncode, HexDecode, hexDecodeInto,
    Base64Encode, Base64Decode, base64DecodeInto,
    Base64URLEncode, Base64URLDecode, base64UrlDecodeInto,
    Base32Encode, Base32Decode, base32DecodeInto,
    Base32HexEncode, Base32HexDecode, base32HexDecodeInto,
    Base85Encode, Base85Decode, base85DecodeInto,
    Base58Encode, Base58Decode, base58DecodeInto,
    Base58CheckEncode, Base58CheckDecode, base58CheckDecodeInto,
    PutUvarint, Uvarint, PutVarint, Varint,
    appendUvarint, uvarintAt, varintAt,
    detectEncoding, DetectEncoding,
    QREncode, QRToString,
} from "dyna:encoding";

let n = 0;
function assert(c, msg) {
    n++;
    if (!c) throw new Error("assertion failed: " + msg);
}
function eq(a, b, msg) { assert(a === b, msg + " (got " + a + ", want " + b + ")"); }
function assertThrowsType(fn, msg, ErrType) {
    let threw = false, err = null;
    try { fn(); } catch (e) { threw = true; err = e; }
    assert(threw, "expected throw: " + msg);
    assert(err instanceof ErrType, msg + " (wrong type: " + err + ")");
}
function u8(...a) { return new Uint8Array(a); }
function join(a) { let s = ""; for (const b of a) s += b + ","; return s; }

let seed = 0x6d5b4c3a;
function rnd() { seed = (seed * 1103515245 + 12345) >>> 0; return seed; }
function randBytes(len) {
    const a = new Uint8Array(len);
    for (let i = 0; i < len; i++) a[i] = rnd() & 0xFF;
    return a;
}
function randLen(max) { return rnd() % (max + 1); }

const VECTORS = 128;

function oracleMatrix(label, encode, oneShotDecode, into, capFor, exactCap) {
    for (let i = 0; i < VECTORS; i++) {
        const data = randBytes(randLen(48));
        const text = encode(data);
        const oracle = oneShotDecode(text);
        const cap = capFor(text.length);
        if (cap < oracle.length)
            throw new Error(label + ": cap " + cap + " below decoded " + oracle.length);
        const minOut = new Uint8Array(cap);
        const w1 = into(text, minOut);
        if (w1 !== oracle.length || join(minOut.subarray(0, oracle.length)) !== join(oracle)) {
            throw new Error(label + " min-cap mismatch at vector " + i +
                            " (w=" + w1 + " got " + join(minOut) + " want " + join(oracle) + ")");
        }
        const big = new Uint8Array(cap + 17).fill(0xAA);
        const w2 = into(text, big);
        let tailOk = true;
        for (let k = cap; k < big.length; k++) if (big[k] !== 0xAA) tailOk = false;
        if (w2 !== oracle.length || join(big.subarray(0, oracle.length)) !== join(oracle) || !tailOk) {
            throw new Error(label + " larger-buffer mismatch at vector " + i);
        }
        if (exactCap) {
            const ex = new Uint8Array(oracle.length).fill(0x55);
            const w3 = into(text, ex);
            if (w3 !== oracle.length || join(ex) !== join(oracle))
                throw new Error(label + " exact-size mismatch at vector " + i);
            n++;
        }
        n += 2;
    }
}

const capHex = (L) => L / 2;
const capB64 = (L) => 3 * (L / 4);
const capB64url = (L) => 3 * Math.floor((L + 3) / 4);
const capB32 = (L) => (L / 8) * 5;
const capB85 = (L) => 4 * L;
const capB58 = (L) => L;

oracleMatrix("hexDecodeInto", HexEncode, HexDecode, hexDecodeInto, capHex, true);
oracleMatrix("base64DecodeInto", Base64Encode, Base64Decode, base64DecodeInto, capB64, false);
oracleMatrix("base64UrlDecodeInto", Base64URLEncode, Base64URLDecode, base64UrlDecodeInto, capB64url, false);
oracleMatrix("base32DecodeInto", Base32Encode, Base32Decode, base32DecodeInto, capB32, false);
oracleMatrix("base32HexDecodeInto", Base32HexEncode, Base32HexDecode, base32HexDecodeInto, capB32, false);
oracleMatrix("base58DecodeInto", Base58Encode, Base58Decode, base58DecodeInto, capB58, false);
oracleMatrix("base58CheckDecodeInto", Base58CheckEncode, Base58CheckDecode, base58CheckDecodeInto, capB58, false);
oracleMatrix("base85DecodeInto", Base85Encode, Base85Decode, base85DecodeInto, capB85, false);

__kitS.section("appendUvarint advances the offset", () => {
    const vals = [0, 1, 127, 128, 300, 16383, 16384, 2 ** 31, 2 ** 32, 2 ** 53 - 1];
    const buf = new Uint8Array(128);
    let off = 0;
    for (const v of vals) {
        const next = appendUvarint(buf, v, off);
        assert(next > off, "appendUvarint advances the offset");
        off = next;
    }
    const endAfterMax = appendUvarint(buf, 2n ** 64n - 1n, off);
    eq(endAfterMax - off, 10, "2^64-1 is the full 10-byte form");
    let pos = 0;
    for (const v of vals) {
        const got = uvarintAt(buf, pos);
        eq(typeof got, "number", "safe magnitudes come back as Number");
        eq(got, v, "walk-back value " + v);
        pos = appendUvarint(buf, v, pos);
    }
    const big = uvarintAt(buf, pos);
    eq(typeof big, "bigint", "2^64-1 comes back as BigInt");
    eq(big, 2n ** 64n - 1n, "2^64-1 round-trips");
    eq(appendUvarint(buf, 300), 2, "appendUvarint default offset 0");
    eq(uvarintAt(u8(0xAC, 0x02)), 300, "uvarintAt default offset 0");
    eq(uvarintAt(u8(0xAC, 0x02), 0), 300, "uvarintAt explicit 0");

    eq(typeof uvarintAt(PutUvarint(2 ** 53 - 1)), "number", "2^53-1 stays Number");
    eq(uvarintAt(PutUvarint(2n ** 53n)), 2n ** 53n, "2^53 becomes BigInt");
    eq(uvarintAt(PutUvarint(2n ** 53n)) + 0n, 9007199254740992n, "2^53 exact");
    eq(uvarintAt(PutUvarint(0)), 0, "zero");

    for (let i = 0; i < VECTORS; i++) {
        const v = i % 3 === 0
            ? BigInt(Math.floor(rnd() % 4294967296)) * 4294967296n + BigInt(rnd() % 4294967296)
            : Math.floor(rnd() % 2 ** 31);
        const [ov, onb] = Uvarint(PutUvarint(v));
        const enc = PutUvarint(v);
        const at = uvarintAt(enc);
        const same = typeof at === "bigint" ? at === ov : at === ov;
        assert(same, "uvarintAt matches Uvarint value for " + v);
        eq(appendUvarint(new Uint8Array(16), v), onb, "appendUvarint end matches bytesRead for " + v);
        n += 3;
    }

    for (let i = 0; i < VECTORS; i++) {
        const v = Math.floor(rnd() % 2 ** 32) - 2 ** 31;
        const [ov] = Varint(PutVarint(v));
        const at = varintAt(PutVarint(v));
        assert(at === ov, "varintAt matches Varint for " + v);
        n += 2;
    }
    eq(varintAt(PutVarint(-1)), -1, "zigzag -1");
    eq(varintAt(PutVarint(2n ** 63n - 1n)), 2n ** 63n - 1n, "int64 max via BigInt");
    const minI64 = varintAt(PutVarint(-(2n ** 63n)));
    eq(minI64, -(2n ** 63n), "int64 minimum round-trips as BigInt");

    eq(uvarintAt(u8(0x80, 0x80, 0x80)), 0, "uvarintAt truncated -> 0");
    eq(uvarintAt(u8()), 0, "uvarintAt empty buffer -> 0");
    eq(varintAt(u8(0x80)), 0, "varintAt truncated -> 0");
    const [tv, tn] = Uvarint(u8(0x80, 0x80, 0x80));
    eq(tv, 0, "oracle: truncated one-shot value half is 0");
    eq(tn, 0, "oracle: truncated one-shot length half is 0");

    const ovf = u8(0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x7f, 0x01);
    assertThrowsType(() => Uvarint(ovf), "one-shot overflow", RangeError);
    assertThrowsType(() => uvarintAt(ovf), "uvarintAt overflow", RangeError);
    assertThrowsType(() => varintAt(ovf), "varintAt overflow", RangeError);
    const ovf2 = u8(0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x02);
    assertThrowsType(() => uvarintAt(ovf2), "final byte above the 64-bit boundary", RangeError);
    eq(uvarintAt(u8(0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01)), 2n ** 64n - 1n,
       "10-byte boundary value accepted");

    const tiny = new Uint8Array(1).fill(0x42);
    assertThrowsType(() => appendUvarint(tiny, 300), "appendUvarint too small", RangeError);
    eq(tiny[0], 0x42, "failed appendUvarint wrote nothing");
    const nearEnd = new Uint8Array(11);
    assertThrowsType(() => appendUvarint(nearEnd, 2n ** 64n - 1n, 2), "10 bytes at offset 2 of 11", RangeError);
    eq(appendUvarint(nearEnd, 2n ** 64n - 1n, 1), 11, "10 bytes at offset 1 of 11 fits exactly");
    assertThrowsType(() => appendUvarint(new Uint8Array(4), 1, 5), "appendUvarint offset past end", RangeError);
    assertThrowsType(() => uvarintAt(u8(1), 2), "uvarintAt offset past end", RangeError);
    eq(uvarintAt(u8(1), 1), 0, "uvarintAt offset == length is an empty stream -> 0");
    assertThrowsType(() => appendUvarint(new Uint8Array(16), -1), "negative value", RangeError);
    assertThrowsType(() => appendUvarint(new Uint8Array(16), 1.5), "fractional value", RangeError);
    assertThrowsType(() => appendUvarint(new Uint8Array(16), 2n ** 64n), "BigInt above 2^64-1", RangeError);
});
__kitS.end();
print("test_encoding_into_at: all tests passed (" + n + " assertions)");
