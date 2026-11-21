// covers: dyna:encoding.DetectEncoding
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_encoding_detect_encoding");
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

__kitS.section("BOM utf-8", () => {
    assert(DetectEncoding(u8(0xEF, 0xBB, 0xBF, 0x61, 0x62)) === "utf-8", "BOM utf-8");
    assert(DetectEncoding(u8(0xFE, 0xFF, 0x00, 0x61)) === "utf-16be", "BOM utf-16be");
    assert(DetectEncoding(u8(0xFF, 0xFE, 0x61, 0x00)) === "utf-16le", "BOM utf-16le");
    assert(DetectEncoding(u8(0x00, 0x00, 0xFE, 0xFF, 0x00, 0x00, 0x00, 0x61)) === "utf-32be", "BOM utf-32be");
    assert(DetectEncoding(u8(0xFF, 0xFE, 0x00, 0x00, 0x61, 0x00, 0x00, 0x00)) === "utf-32le", "BOM utf-32le");

    assert(DetectEncoding(u8(0x61, 0x62, 0x63, 0x64)) === "utf-8", "ASCII is utf-8");
    assert(DetectEncoding(new TextEncoder().encode("Hello 世界! Привет мир!")) === "utf-8", "valid UTF-8");

    const gbkSample = u8(0xD6, 0xD0, 0xCE, 0xC4, 0xB1, 0xE0, 0xC2, 0xEB, 0xB2, 0xE2, 0xCA, 0xD4, 0x20, 0x31, 0x32, 0x33);
    assert(DetectEncoding(gbkSample) === "gbk", "detect GBK");

    const sjisSample = u8(0x93, 0xFA, 0x96, 0x7B, 0x8C, 0xEA, 0x83, 0x65, 0x83, 0x58, 0x83, 0x67, 0x82, 0xC5, 0x82, 0xB7);
    assert(DetectEncoding(sjisSample) === "shift_jis", "detect Shift-JIS");

    const eucjpSample = u8(0x8E, 0xA1, 0x8E, 0xA2, 0x8E, 0xA3, 0x8E, 0xA4, 0x8E, 0xA5, 0x8E, 0xA6, 0x8E, 0xA7, 0x8E, 0xA8);
    assert(DetectEncoding(eucjpSample) === "euc-jp", "detect EUC-JP half-width kana");

    assert(detectEncoding(u8(0x61, 0x62), { fallback: "windows-1252" }) === "utf-8", "ASCII respects utf-8 first");
    assert(DetectEncoding(gbkSample, { allowList: ["gbk", "utf-8"] }) === "gbk", "allowList hit");
    assert(DetectEncoding(gbkSample, { allowList: ["utf-8", "shift_jis"], fallback: "utf-8" }) === "utf-8", "allowList fallback");
    assertThrows(() => DetectEncoding(gbkSample, { allowList: ["shift_jis"] }), "allowList rejection throws", TypeError);

    assertThrows(() => DetectEncoding("not a buffer"), "non-buffer throws", TypeError);
    assertThrows(() => DetectEncoding(null), "null throws", TypeError);
});
__kitS.end();

print("test_encoding: all tests passed (" + n + " assertions)");
