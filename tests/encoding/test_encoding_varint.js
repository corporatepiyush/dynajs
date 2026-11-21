// covers: dyna:encoding.Varint
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_encoding_varint");
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

__kitS.section("PutVarint(-1) -> zigzag 1 -> [1]", () => {
    assert(eqArr(PutVarint(-1), u8(1)), "PutVarint(-1) -> zigzag 1 -> [1]");
    assert(eqArr(PutVarint(1), u8(2)), "PutVarint(1) -> zigzag 2 -> [2]");
    assert(eqArr(PutVarint(0), u8(0)), "PutVarint(0) -> [0]");
    assert(eqArr(PutVarint(-2), u8(3)), "PutVarint(-2) -> zigzag 3 -> [3]");
    assert(eqArr(PutVarint(2), u8(4)), "PutVarint(2) -> zigzag 4 -> [4]");

    {
        let [v, nb] = Varint(u8(1));
        assert(v === -1 && nb === 1, "Varint([1]) -> [-1, 1]");
    }
    {
        let [v, nb] = Varint(u8(2));
        assert(v === 1 && nb === 1, "Varint([2]) -> [1, 1]");
    }
    {
        let [v, nb] = Varint(u8(0));
        assert(v === 0 && nb === 1, "Varint([0]) -> [0, 1]");
    }

    assertThrows(() => PutVarint(1.5), "PutVarint: non-integer Number rejected", RangeError);
    assertThrows(() => PutVarint(Number.MAX_SAFE_INTEGER + 1),
        "PutVarint: Number beyond safe-integer range rejected", RangeError);
    assertThrows(() => PutVarint(-(Number.MAX_SAFE_INTEGER) - 1),
        "PutVarint: Number beyond -safe-integer range rejected", RangeError);

    assert(eqArr(PutVarint(-9007199254740992n),
        u8(255, 255, 255, 255, 255, 255, 255, 31)), "PutVarint(-2^53 BigInt)");
    {
        let [v, nb] = Varint(u8(255, 255, 255, 255, 255, 255, 255, 31));
        assert(typeof v === "bigint", "Varint: value beyond safe range comes back as BigInt");
        assert(v === -9007199254740992n && nb === 8, "Varint: -2^53 roundtrip via BigInt");
    }
    {
        const enc = PutVarint(-(Number.MAX_SAFE_INTEGER));
        const [v, nb] = Varint(enc);
        assert(typeof v === "number", "Varint: -MIN_SAFE_INTEGER-ish still comes back as Number");
        assert(v === -(Number.MAX_SAFE_INTEGER), "Varint: -(2^53-1) exact roundtrip");
    }

    {
        let [v, nb] = Varint(u8());
        assert(v === 0 && nb === 0, "Varint([]) -> [0, 0] (truncated)");
    }
    {
        let [v, nb] = Varint(u8(0x80, 0x80));
        assert(v === 0 && nb === 0, "Varint([0x80,0x80]) -> [0, 0] (truncated)");
    }
    {
        const overflow11 = new Uint8Array(11).fill(0x80);
        assert_throws(() => Varint(overflow11),
            "Varint: overflow follows Uvarint's RangeError contract");
    }

    const varintCases = [0, 1, -1, 2, -2, 63, -64, 64, -65, 1000000, -1000000,
        Number.MAX_SAFE_INTEGER, -Number.MAX_SAFE_INTEGER];
    for (const x of varintCases) {
        const enc = PutVarint(x);
        const [v, nb] = Varint(enc);
        assert(v === x && nb === enc.length, "Varint roundtrip: " + x);
    }
    const bigSignedCases = [0n, -1n, 1n, -9007199254740992n, 9007199254740992n,
        -(1n << 62n), (1n << 62n)];
    for (const x of bigSignedCases) {
        const enc = PutVarint(x);
        const [v, nb] = Varint(enc);
        const vBig = typeof v === "bigint" ? v : BigInt(v);
        assert(vBig === x && nb === enc.length, "Varint BigInt roundtrip: " + x);
    }
});
__kitS.end();

print("test_encoding: all tests passed (" + n + " assertions)");
