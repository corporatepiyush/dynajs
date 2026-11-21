// covers: dyna:encoding.Base64URLDecode
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_encoding_base64_url_decode_into_at");
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

__kitS.section("b64url roundtrip bytes", () => {
    for (let i = 0; i < 64; i++) {
        const s = "payload-" + i + "-" + String.fromCharCode(0x20 + (rnd() % 90));
        const bytes = u8(...s.split("").map((c) => c.charCodeAt(0) & 0xFF));
        const back = Base64URLDecode(Base64URLEncode(bytes));
        eq(join(back), join(bytes), "b64url roundtrip bytes");
        n++;
    }
});
__kitS.end();
print("test_encoding_into_at: all tests passed (" + n + " assertions)");
