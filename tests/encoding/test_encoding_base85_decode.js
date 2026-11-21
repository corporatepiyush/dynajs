// covers: dyna:encoding.Base85Decode
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_encoding_base85_decode");
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

__kitS.section("Base85Encode: empty -> empty", () => {
    assert(Base85Encode("") === "", "Base85Encode: empty -> empty");
    assert(Base85Decode("").length === 0, "Base85Decode: empty -> empty");

    for (let i = 0; i < WORDS.length; i++) {
        assert(Base85Encode(WORDS[i]) === B85_OF_WORD[i], "Base85Encode word[" + i + "]");
        assert(bytesToStr(Base85Decode(B85_OF_WORD[i])) === WORDS[i], "Base85Decode word[" + i + "]");
    }
    assert(Base85Encode(RANDOM53) === RANDOM53_B85, "Base85Encode: 53 random bytes vs python oracle");
    assert(eqArr(Base85Decode(RANDOM53_B85), RANDOM53), "Base85Decode: 53 random bytes vs python oracle");

    assert(Base85Encode(u8(0, 0, 0, 0)) === "z", "Base85Encode: all-zero 4-byte group -> 'z'");
    assert(Base85Encode(u8(0, 0, 0, 0, 0, 0, 0, 0)) === "zz", "Base85Encode: two all-zero groups -> 'zz'");
    assert(eqArr(Base85Decode("z"), u8(0, 0, 0, 0)), "Base85Decode: 'z' -> 4 zero bytes");
    assert(eqArr(Base85Decode("zz"), u8(0, 0, 0, 0, 0, 0, 0, 0)), "Base85Decode: 'zz' -> 8 zero bytes");
    assert(Base85Encode(u8(0, 0, 0)) !== "z", "Base85Encode: partial all-zero group does not use 'z'");
    assert(eqArr(Base85Decode(Base85Encode(u8(0, 0, 0))), u8(0, 0, 0)), "base85 roundtrip: partial all-zero group");

    assert(eqArr(Base85Decode("Ao DS"), u8(0x66, 0x6f, 0x6f)), "Base85Decode: embedded space skipped");
    assert(eqArr(Base85Decode("Ao\nDS\t"), u8(0x66, 0x6f, 0x6f)), "Base85Decode: embedded newline/tab skipped");

    assertThrows(() => Base85Decode("v"), "Base85Decode: byte above 'u' is invalid", SyntaxError);
    assertThrows(() => Base85Decode(String.fromCharCode(31)),
        "Base85Decode: byte below '!' (and not whitespace) is invalid", SyntaxError);
    assertThrows(() => Base85Decode("Ao@Sz"), "Base85Decode: 'z' off a group boundary is invalid", SyntaxError);
    assertThrows(() => Base85Decode("A"), "Base85Decode: a single leftover char is impossible", SyntaxError);
    assertThrows(() => Base85Decode("AoDTsA"), "Base85Decode: 1 leftover char after full groups is impossible", SyntaxError);

    for (let len = 0; len <= 64; len++) {
        const b = randBytes(len);
        assert(eqArr(Base85Decode(Base85Encode(b)), b), "base85 roundtrip fuzz len=" + len);
    }
    for (let len = 0; len <= 20; len++) {
        const b = new Uint8Array(len);
        assert(eqArr(Base85Decode(Base85Encode(b)), b), "base85 all-zero roundtrip len=" + len);
    }

    assert(eqArr(Base85Decode("s8W-!"), u8(0xFF, 0xFF, 0xFF, 0xFF)),
        "Base85Decode: 's8W-!' = largest in-range full group -> ff ff ff ff (boundary still accepted)");
    assertThrows(() => Base85Decode('s8W-"'),
        "Base85Decode: s8W-quot (first group naming 0x100000000) throws, python Ascii85 overflow parity", SyntaxError);
    assertThrows(() => Base85Decode("uuuuu"),
        "Base85Decode: 'uuuuu' (85^5-1, all-max digits) throws", SyntaxError);
    assertThrows(() => Base85Decode('s8W-!s8W-"'),
        "Base85Decode: overflow AFTER valid groups throws", SyntaxError);
    assertThrows(() => Base85Decode('s8W-"s8W-!'),
        "Base85Decode: overflow BEFORE a valid group throws", SyntaxError);

    const CORPUS510_HEX = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f404142434445464748494a4b4c4d4e4f505152535455565758595a5b5c5d5e5f606162636465666768696a6b6c6d6e6f707172737475767778797a7b7c7d7e7f808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9fa0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9babbbcbdbebfc0c1c2c3c4c5c6c7c8c9cacbcccdcecfd0d1d2d3d4d5d6d7d8d9dadbdcdddedfe0e1e2e3e4e5e6e7e8e9eaebecedeeeff0f1f2f3f4f5f6f7f8f9fafbfcfdfeff000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f404142434445464748494a4b4c4d4e4f505152535455565758595a5b5c5d5e5f606162636465666768696a6b6c6d6e6f707172737475767778797a7b7c7d7e7f808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9fa0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b7b8b9babbbcbdbebfc0c1c2c3c4c5c6c7c8c9cacbcccdcecfd0d1d2d3d4d5d6d7d8d9dadbdcdddedfe0e1e2e3e4e5e6e7e8e9eaebecedeeeff0f1f2f3f4f5f6f7f8f9fafbfcfd";
    const CORPUS510_B85 = "!!*-'\"9eu7#RLhG$k3[W&.oNg'GVB\"(`=52*$$(B+<_pR,UFcb-n-Vr/1iJ-0JP==1c70M3&s#]4?Ykm5X@_(6q'R884cEH9MJ8X:f1+h<)lt#=BSg3>[:ZC?t!MSA7]@cBPD3sCi+'.E,fo>FEMbNG^4U^I!pHnJ:W<)KS>/9Ll%\"IN/`jYOHG]iPa.Q$R$jD4S=Q7DTV8*TUnsrdW2ZetXKAY/Yd(L?['d?O\\@K2_]Y2%o^qmn*`5Ta:aN;TJbg\"GZd*^:jeCE.%f\\,!5gtgiEi8N\\UjQ5OekiqBum-X60nF?)@o_%qPq\"ad`r;HWp!!*-'\"9eu7#RLhG$k3[W&.oNg'GVB\"(`=52*$$(B+<_pR,UFcb-n-Vr/1iJ-0JP==1c70M3&s#]4?Ykm5X@_(6q'R884cEH9MJ8X:f1+h<)lt#=BSg3>[:ZC?t!MSA7]@cBPD3sCi+'.E,fo>FEMbNG^4U^I!pHnJ:W<)KS>/9Ll%\"IN/`jYOHG]iPa.Q$R$jD4S=Q7DTV8*TUnsrdW2ZetXKAY/Yd(L?['d?O\\@K2_]Y2%o^qmn*`5Ta:aN;TJbg\"GZd*^:jeCE.%f\\,!5gtgiEi8N\\UjQ5OekiqBum-X60nF?)@o_%qPq\"ad`r;?";
    const corpus510 = new Uint8Array(CORPUS510_HEX.length / 2);
    for (let i = 0; i < corpus510.length; i++)
        corpus510[i] = parseInt(CORPUS510_HEX.substring(i * 2, i * 2 + 2), 16);
    assert(Base85Encode(corpus510) === CORPUS510_B85,
        "Base85Encode: 510-byte corpus (bytes(range(256))*2)[:510] vs python a85encode oracle");
    assert(eqArr(Base85Decode(CORPUS510_B85), corpus510),
        "Base85Decode: 510-byte corpus vs python a85decode oracle");

    const ALL_FF_B85 = ["", "rr", "s8N", "s8W*", "s8W-!",
        "s8W-!rr", "s8W-!s8N", "s8W-!s8W*", "s8W-!s8W-!"];
    for (let len = 0; len <= 8; len++) {
        const b = new Uint8Array(len).fill(0xFF);
        assert(Base85Encode(b) === ALL_FF_B85[len],
            "base85 all-0xFF encode len=" + len + " vs python oracle");
        assert(eqArr(Base85Decode(Base85Encode(b)), b),
            "base85 all-0xFF roundtrip len=" + len);
    }
});

__kitS.section("a85 overlong partial group refused", () => {
    assertThrows(() => Base85Decode("s8"), "a85 overlong partial group refused", SyntaxError);
    assertThrows(() => Base85Decode("uu"), "a85 overlong partial (84*85^4 > 2^32-1) refused", SyntaxError);
    eqArr(Array.from(Base85Decode("s8N")), [255, 255], "a85 partial at the 2^32-1 boundary still decodes");
});
__kitS.end();

print("test_encoding: all tests passed (" + n + " assertions)");
