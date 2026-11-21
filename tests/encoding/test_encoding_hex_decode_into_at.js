// covers: dyna:encoding.hexDecodeInto
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_encoding_hex_decode_into_at");
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

__kitS.section("hex odd", () => {
    const cases = [
        ["hex odd",     () => HexDecode("abc"),            () => hexDecodeInto("abc", new Uint8Array(8))],
        ["hex digit",   () => HexDecode("abzz"),           () => hexDecodeInto("abzz", new Uint8Array(8))],
        ["b64 space",   () => Base64Decode(" Zm9v "),      () => base64DecodeInto(" Zm9v ", new Uint8Array(8))],
        ["b64 bang",    () => Base64Decode("Zm9!"),        () => base64DecodeInto("Zm9!", new Uint8Array(8))],
        ["b64 unpadded 3", () => Base64Decode("QQQ"),      () => base64DecodeInto("QQQ", new Uint8Array(8))],
        ["b64 unpadded 2", () => Base64Decode("QQ"),       () => base64DecodeInto("QQ", new Uint8Array(8))],
        ["b32 12 unpadded", () => Base32Decode("MFRGGZDFMZTW"), () => base32DecodeInto("MFRGGZDFMZTW", new Uint8Array(16))],
        ["b64url plus", () => Base64URLDecode("a+b/"),     () => base64UrlDecodeInto("a+b/", new Uint8Array(8))],
        ["b64url 4k+1", () => Base64URLDecode("abcde"),    () => base64UrlDecodeInto("abcde", new Uint8Array(8))],
        ["b32 unpadded",() => Base32Decode("ABCDEFG"),     () => base32DecodeInto("ABCDEFG", new Uint8Array(8))],
        ["b32 char",    () => Base32Decode("A1AAAAAA"),    () => base32DecodeInto("A1AAAAAA", new Uint8Array(8))],
        ["b32hex char", () => Base32HexDecode("AWAAAAAA"), () => base32HexDecodeInto("AWAAAAAA", new Uint8Array(8))],
        ["b85 overflow",() => Base85Decode("s8W-\""),      () => base85DecodeInto("s8W-\"", new Uint8Array(20))]  ,
        ["b85 stray z", () => Base85Decode("aazaa"),       () => base85DecodeInto("aazaa", new Uint8Array(20))],
        ["b58 char",    () => Base58Decode("0OIl"),        () => base58DecodeInto("0OIl", new Uint8Array(8))],
        ["b58ck short", () => Base58CheckDecode("ab"),     () => base58CheckDecodeInto("ab", new Uint8Array(8))],
    ];
    for (const [name, oneShot, into] of cases) {
        let t1 = null, t2 = null;
        try { oneShot(); } catch (e) { t1 = e.constructor.name; }
        try { into(); } catch (e) { t2 = e.constructor.name; }
        assert(t1 !== null, name + ": one-shot throws");
        eq(t2, t1, name + ": Into error parity");
    }

    const buf = new Uint8Array(3).fill(0x5A);
    assertThrowsType(() => hexDecodeInto("deadbeef", buf), "hex into too small", RangeError);
    eq(join(buf), "90,90,90,", "buffer untouched after capacity RangeError");
    assertThrowsType(() => base64DecodeInto("Zm9vZm9v", new Uint8Array(3)), "b64 into too small", RangeError);
    assertThrowsType(() => base85DecodeInto("s8W-!", new Uint8Array(3)), "b85 into too small", RangeError);
    assertThrowsType(() => base58DecodeInto("abc", new Uint8Array(2)), "b58 into too small", RangeError);
    const dv = new DataView(new ArrayBuffer(4));
    eq(hexDecodeInto("deadbeef", dv), 4, "DataView out accepted");
    eq(dv.getUint8(0), 0xde, "DataView out written");
    assertThrowsType(() => hexDecodeInto("ab", 5), "junk out", TypeError);
    eq(hexDecodeInto("", new Uint8Array(4)), 0, "hex into empty");
    eq(base58DecodeInto("", new Uint8Array(4)), 0, "b58 into empty");
});
__kitS.end();
print("test_encoding_into_at: all tests passed (" + n + " assertions)");
