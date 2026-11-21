// covers: dyna:encoding.PutUvarint
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_encoding_put_uvarint");
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

__kitS.section("PutUvarint(0) -> [0]", () => {
    assert(eqArr(PutUvarint(0), u8(0)), "PutUvarint(0) -> [0]");
    assert(eqArr(PutUvarint(300), u8(0xAC, 0x02)), "PutUvarint(300) -> [0xAC, 0x02]");
    assert(eqArr(PutUvarint(1), u8(1)), "PutUvarint(1) -> [1]");
    assert(eqArr(PutUvarint(127), u8(127)), "PutUvarint(127) -> [127] (1-byte boundary)");
    assert(eqArr(PutUvarint(128), u8(128, 1)), "PutUvarint(128) -> [0x80, 0x01] (2-byte boundary)");
    assert(eqArr(PutUvarint(16384), u8(128, 128, 1)), "PutUvarint(16384) -> 3-byte boundary");
    assert(eqArr(PutUvarint(4294967296), u8(128, 128, 128, 128, 16)), "PutUvarint(2^32) -> 5 bytes");
    assert(eqArr(PutUvarint(Number.MAX_SAFE_INTEGER),
        u8(255, 255, 255, 255, 255, 255, 255, 15)), "PutUvarint(2^53-1) -> 8 bytes");

    {
        let [v, nb] = Uvarint(u8(0));
        assert(v === 0 && nb === 1, "Uvarint([0]) -> [0, 1]");
    }
    {
        let [v, nb] = Uvarint(u8(0xAC, 0x02));
        assert(v === 300 && nb === 2, "Uvarint([0xAC,0x02]) -> [300, 2]");
    }
    {
        let [v, nb] = Uvarint(u8(0xAC, 0x02, 0xFF, 0xFF));
        assert(v === 300 && nb === 2, "Uvarint stops after its own Varint, ignoring trailing bytes");
    }

    assert(eqArr(PutUvarint(9007199254740992n),
        u8(128, 128, 128, 128, 128, 128, 128, 16)), "PutUvarint(2^53 as BigInt) -> 8 bytes");
    assert(eqArr(PutUvarint(18446744073709551615n),
        u8(255, 255, 255, 255, 255, 255, 255, 255, 255, 1)), "PutUvarint(2^64-1 BigInt) -> 10 bytes (max)");
    {
        let [v, nb] = Uvarint(u8(255, 255, 255, 255, 255, 255, 255, 255, 255, 1));
        assert(typeof v === "bigint", "Uvarint: value beyond 2^53 comes back as BigInt");
        assert(v === 18446744073709551615n && nb === 10, "Uvarint: 2^64-1 roundtrip via BigInt");
    }
    {
        let [v, nb] = Uvarint(u8(255, 255, 255, 255, 255, 255, 255, 15));
        assert(typeof v === "number", "Uvarint: MAX_SAFE_INTEGER still comes back as Number");
        assert(v === Number.MAX_SAFE_INTEGER && nb === 8, "Uvarint: 2^53-1 exact roundtrip");
    }

    assertThrows(() => PutUvarint(-1), "PutUvarint: negative Number rejected", RangeError);
    assertThrows(() => PutUvarint(1.5), "PutUvarint: non-integer Number rejected", RangeError);
    assertThrows(() => PutUvarint(Number.MAX_SAFE_INTEGER + 1),
        "PutUvarint: Number beyond 2^53-1 rejected (must use BigInt)", RangeError);

    {
        let [v, nb] = Uvarint(u8());
        assert(v === 0 && nb === 0, "Uvarint([]) -> [0, 0] (truncated: empty)");
    }
    {
        let [v, nb] = Uvarint(u8(0x80));
        assert(v === 0 && nb === 0, "Uvarint([0x80]) -> [0, 0] (truncated: no terminator)");
    }
    {
        let [v, nb] = Uvarint(u8(0x80, 0x80, 0x80));
        assert(v === 0 && nb === 0, "Uvarint([0x80,0x80,0x80]) -> [0, 0] (truncated)");
    }
    {
        const overflow11 = new Uint8Array(11).fill(0x80);
        assert_throws(() => Uvarint(overflow11),
            "Uvarint: 11 continuation bytes overflow -> RangeError");
    }
    {
        const overflow10 = u8(0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x02);
        assert_throws(() => Uvarint(overflow10),
            "Uvarint: final byte > 1 at the 10-byte boundary overflows -> RangeError");
    }

    const uvarintCases = [0, 1, 2, 127, 128, 129, 16383, 16384, 300, 65535, 65536,
        2097151, 2097152, 268435455, 268435456, 4294967296, 34359738368,
        Number.MAX_SAFE_INTEGER, Number.MAX_SAFE_INTEGER - 1];
    for (const x of uvarintCases) {
        const enc = PutUvarint(x);
        const [v, nb] = Uvarint(enc);
        assert(v === x && nb === enc.length, "Uvarint roundtrip: " + x);
    }
    const bigCases = [0n, 1n, 300n, 9007199254740991n, 9007199254740992n,
        (1n << 63n), (1n << 64n) - 1n];
    for (const x of bigCases) {
        const enc = PutUvarint(x);
        const [v, nb] = Uvarint(enc);
        const vBig = typeof v === "bigint" ? v : BigInt(v);
        assert(vBig === x && nb === enc.length, "Uvarint BigInt roundtrip: " + x);
    }
});

__kitS.section("PutUvarint: accepts a valueOf-coercible object", () => {
    assert(eqArr(PutUvarint({ valueOf() { return 300; } }), u8(0xAC, 0x02)),
        "PutUvarint: accepts a valueOf-coercible object");
    assert(eqArr(PutVarint({ valueOf() { return -1; } }), u8(1)),
        "PutVarint: accepts a valueOf-coercible object");
    assertThrows(() => HexEncode(new String("abc")),
        "HexEncode: String wrapper object (boxed) is rejected, not silently unboxed", TypeError);
});
__kitS.end();

print("test_encoding: all tests passed (" + n + " assertions)");
