// covers: dyna:encoding.HexEncode
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_encoding_hex_encode");
import {
    HexEncode, HexDecode,
    Base64Encode, Base64Decode, Base64URLEncode, Base64URLDecode,
    Base32Encode, Base32Decode, Base32HexEncode, Base32HexDecode,
    PutUvarint, Uvarint, PutVarint, Varint,
    Base85Encode, Base85Decode,
    DetectEncoding, detectEncoding,
} from "dyna:encoding";

let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
}
function assert_throws(fn, msg) {
    n++;
    try { fn(); } catch (e) { return; }
    throw new Error("assertion failed (no throw): " + msg);
}
function eqArr(a, b) {
    if (a.length !== b.length) return false;
    for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
    return true;
}
function assertThrows(fn, msg, ErrType) {
    n++;
    let threw = false, err = null;
    try { fn(); } catch (e) { threw = true; err = e; }
    if (!threw) throw new Error("assertion failed (expected throw): " + msg);
    if (ErrType && !(err instanceof ErrType))
        throw new Error("assertion failed (wrong error type, got " + err + "): " + msg);
}
function u8(...bytes) { return new Uint8Array(bytes); }
function bytesToStr(u8arr) {
    let s = "";
    for (const b of u8arr) s += String.fromCharCode(b);
    return s;
}

let seed = 0x2f6e2b1;
function rnd() { seed = (seed * 1103515245 + 12345) >>> 0; return seed; }
function randBytes(len) {
    const a = new Uint8Array(len);
    for (let i = 0; i < len; i++) a[i] = rnd() & 0xFF;
    return a;
}

const WORDS = ["", "f", "fo", "foo", "foob", "fooba", "foobar"];
const HEX_OF_WORD = ["", "66", "666f", "666f6f", "666f6f62", "666f6f6261", "666f6f626172"];
const B64_OF_WORD = ["", "Zg==", "Zm8=", "Zm9v", "Zm9vYg==", "Zm9vYmE=", "Zm9vYmFy"];
const B64URL_OF_WORD = ["", "Zg", "Zm8", "Zm9v", "Zm9vYg", "Zm9vYmE", "Zm9vYmFy"];
const B32_OF_WORD = ["", "MY======", "MZXQ====", "MZXW6===", "MZXW6YQ=", "MZXW6YTB", "MZXW6YTBOI======"];
const B32HEX_OF_WORD = ["", "CO======", "CPNG====", "CPNMU===", "CPNMUOG=", "CPNMUOJ1", "CPNMUOJ1E8======"];
const B85_OF_WORD = ["", "Ac", "Ao@", "AoDS", "AoDTs", "AoDTs@/", "AoDTs@<)"];

const RANDOM37 = u8(57,12,140,125,114,71,52,44,216,16,15,47,111,119,13,101,214,
    112,229,142,3,81,216,174,142,79,110,172,52,47,194,49,183,176,135,22,235);
const RANDOM37_HEX = "390c8c7d7247342cd8100f2f6f770d65d670e58e0351d8ae8e4f6eac342fc231b7b08716eb";
const RANDOM37_B64 = "OQyMfXJHNCzYEA8vb3cNZdZw5Y4DUdiujk9urDQvwjG3sIcW6w==";
const RANDOM37_B64URL = "OQyMfXJHNCzYEA8vb3cNZdZw5Y4DUdiujk9urDQvwjG3sIcW6w";
const RANDOM37_B32 = "HEGIY7LSI42CZWAQB4XW65YNMXLHBZMOANI5RLUOJ5XKYNBPYIY3PMEHC3VQ====";
const RANDOM37_B32HEX = "7468OVBI8SQ2PM0G1SNMUTODCNB71PCE0D8THBKE9TNAOD1FO8ORFC472RLG====";

const RANDOM53 = u8(206,194,102,91,117,127,68,44,128,196,45,250,102,215,110,191,
    198,108,77,236,91,173,42,44,240,161,206,21,137,3,217,104,185,41,236,101,236,
    26,183,66,110,82,226,90,90,18,56,11,87,242,214,41,217);
const RANDOM53_B85 = "cGF0tFale1JAa&9B%Lq8`b\\B9>HKA+n=e,BM$)FE\\LHlVll*?\"DGoIt=qWe&=7Xokf`";

__kitS.section("HexEncode: empty string -> empty", () => {
    assert(HexEncode("") === "", "HexEncode: empty string -> empty");
    assert(HexEncode(u8()) === "", "HexEncode: empty Uint8Array -> empty");
    assert(HexEncode("abc") === "616263", "HexEncode: 'abc' (utf8) -> '616263'");
    assert(HexEncode(u8(0x61, 0x62, 0x63)) === "616263", "HexEncode: Uint8Array [0x61,0x62,0x63]");
    assert(HexEncode(new ArrayBuffer(0)) === "", "HexEncode: empty ArrayBuffer -> empty");

    assert(HexDecode("") .length === 0, "HexDecode: '' -> empty Uint8Array");
    assert(eqArr(HexDecode("616263"), u8(0x61, 0x62, 0x63)), "HexDecode: '616263' -> [0x61,0x62,0x63]");
    assert(bytesToStr(HexDecode(HexEncode("abc"))) === "abc", "hex roundtrip: 'abc'");
    assert(eqArr(HexDecode("6162"), HexDecode("6162")), "HexDecode sanity self-eq");
    assert(eqArr(HexDecode("DEAD"), u8(0xDE, 0xAD)), "HexDecode: uppercase hex digits");
    assert(eqArr(HexDecode("DeAd"), u8(0xDE, 0xAD)), "HexDecode: mixed-case hex digits");

    for (let i = 0; i < WORDS.length; i++) {
        assert(HexEncode(WORDS[i]) === HEX_OF_WORD[i], "HexEncode RFC word[" + i + "]");
        assert(bytesToStr(HexDecode(HEX_OF_WORD[i])) === WORDS[i], "HexDecode RFC word[" + i + "]");
    }
    assert(HexEncode(RANDOM37) === RANDOM37_HEX, "HexEncode: 37 random bytes vs python oracle");
    assert(eqArr(HexDecode(RANDOM37_HEX), RANDOM37), "HexDecode: 37 random bytes vs python oracle");

    assertThrows(() => HexDecode("abc"), "HexDecode: odd-length throws", SyntaxError);
    assertThrows(() => HexDecode("gg"), "HexDecode: invalid digit 'g' throws", SyntaxError);
    assertThrows(() => HexDecode("0g"), "HexDecode: invalid digit in 2nd nibble throws", SyntaxError);
    assertThrows(() => HexDecode("g0"), "HexDecode: invalid digit in 1st nibble throws", SyntaxError);

    assertThrows(() => HexEncode(42), "HexEncode: Number input throws TypeError", TypeError);
    assertThrows(() => HexEncode({}), "HexEncode: plain object input throws TypeError", TypeError);
    assertThrows(() => HexEncode(null), "HexEncode: null input throws TypeError", TypeError);
    assertThrows(() => HexEncode([1, 2, 3]), "HexEncode: plain Array input throws TypeError", TypeError);
    assertThrows(() => HexEncode(new Uint32Array([1, 2])), "HexEncode: Uint32Array rejected", TypeError);

    for (let len = 0; len <= 64; len++) {
        const b = randBytes(len);
        assert(eqArr(HexDecode(HexEncode(b)), b), "hex roundtrip fuzz len=" + len);
    }
});
__kitS.end();

print("test_encoding: all tests passed (" + n + " assertions)");
