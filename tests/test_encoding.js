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

{
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
}

{
    assert(Base64Encode("") === "", "Base64Encode: empty -> empty");
    assert(Base64Decode("").length === 0, "Base64Decode: empty -> empty");

    for (let i = 0; i < WORDS.length; i++) {
        assert(Base64Encode(WORDS[i]) === B64_OF_WORD[i], "Base64Encode RFC word[" + i + "]");
        assert(bytesToStr(Base64Decode(B64_OF_WORD[i])) === WORDS[i], "Base64Decode RFC word[" + i + "]");
    }
    assert(Base64Encode(RANDOM37) === RANDOM37_B64, "Base64Encode: 37 random bytes vs python oracle");
    assert(eqArr(Base64Decode(RANDOM37_B64), RANDOM37), "Base64Decode: 37 random bytes vs python oracle");

    assert(Base64Encode(u8(0x66, 0x6f, 0x6f)) === "Zm9v", "Base64Encode: Uint8Array input");
    assert(Base64Encode(u8(0x66, 0x6f, 0x6f).buffer) === "Zm9v", "Base64Encode: ArrayBuffer input");

    assertThrows(() => Base64Decode("Zm9v!"), "Base64Decode: bad length throws", SyntaxError);
    assertThrows(() => Base64Decode("!!!!"), "Base64Decode: invalid chars throws", SyntaxError);
    assertThrows(() => Base64Decode("Z==="), "Base64Decode: misplaced padding throws", SyntaxError);

    for (let len = 0; len <= 64; len++) {
        const b = randBytes(len);
        assert(eqArr(Base64Decode(Base64Encode(b)), b), "base64 roundtrip fuzz len=" + len);
    }
}

{
    assert(Base64URLEncode("") === "", "Base64URLEncode: empty -> empty");
    assert(Base64URLDecode("").length === 0, "Base64URLDecode: empty -> empty");

    for (let i = 0; i < WORDS.length; i++) {
        assert(Base64URLEncode(WORDS[i]) === B64URL_OF_WORD[i], "Base64URLEncode RFC word[" + i + "]");
        assert(bytesToStr(Base64URLDecode(B64URL_OF_WORD[i])) === WORDS[i], "Base64URLDecode RFC word[" + i + "]");
        assert(!Base64URLEncode(WORDS[i]).includes("="), "Base64URLEncode never pads: word[" + i + "]");
    }
    assert(Base64URLEncode(RANDOM37) === RANDOM37_B64URL, "Base64URLEncode: 37 random bytes vs python oracle");
    assert(eqArr(Base64URLDecode(RANDOM37_B64URL), RANDOM37), "Base64URLDecode: 37 random bytes vs python oracle");

    assert(Base64Encode(u8(0xF8)) === "+A==", "Base64Encode: byte 0xF8 hits alphabet[62] '+'");
    assert(Base64URLEncode(u8(0xF8)) === "-A", "Base64URLEncode: byte 0xF8 hits alphabet[62] '-', unpadded");
    assert(Base64Encode(u8(0xFC)) === "/A==", "Base64Encode: byte 0xFC hits alphabet[63] '/'");
    assert(Base64URLEncode(u8(0xFC)) === "_A", "Base64URLEncode: byte 0xFC hits alphabet[63] '_', unpadded");
    assert(eqArr(Base64URLDecode("-A"), u8(0xF8)), "Base64URLDecode: '-A' -> [0xF8]");
    assert(eqArr(Base64URLDecode("_A"), u8(0xFC)), "Base64URLDecode: '_A' -> [0xFC]");

    assertThrows(() => Base64URLDecode("+A"), "Base64URLDecode: rejects '+' (not url-safe alphabet)", SyntaxError);
    assertThrows(() => Base64URLDecode("/A"), "Base64URLDecode: rejects '/' (not url-safe alphabet)", SyntaxError);
    assert(eqArr(Base64URLDecode("Zg=="), u8(0x66)), "Base64URLDecode: tolerates explicit '=' padding");

    for (let len = 0; len <= 64; len++) {
        const b = randBytes(len);
        const enc = Base64URLEncode(b);
        assert(!enc.includes("="), "Base64URLEncode never pads (len=" + len + ")");
        assert(eqArr(Base64URLDecode(enc), b), "base64url roundtrip fuzz len=" + len);
    }
}

{
    assert(Base32Encode("") === "", "Base32Encode: empty -> empty");
    assert(Base32Decode("").length === 0, "Base32Decode: empty -> empty");
    assert(Base32HexEncode("") === "", "Base32HexEncode: empty -> empty");
    assert(Base32HexDecode("").length === 0, "Base32HexDecode: empty -> empty");

    for (let i = 0; i < WORDS.length; i++) {
        assert(Base32Encode(WORDS[i]) === B32_OF_WORD[i], "Base32Encode RFC word[" + i + "]");
        assert(bytesToStr(Base32Decode(B32_OF_WORD[i])) === WORDS[i], "Base32Decode RFC word[" + i + "]");
        assert(Base32HexEncode(WORDS[i]) === B32HEX_OF_WORD[i], "Base32HexEncode RFC word[" + i + "]");
        assert(bytesToStr(Base32HexDecode(B32HEX_OF_WORD[i])) === WORDS[i], "Base32HexDecode RFC word[" + i + "]");
    }
    assert(Base32Encode("f") === "MY======", "Base32Encode('f') RFC vector");
    assert(Base32Encode("fo") === "MZXQ====", "Base32Encode('fo') RFC vector");
    assert(Base32Encode("foobar") === "MZXW6YTBOI======", "Base32Encode('foobar') RFC vector");

    assert(Base32Encode(RANDOM37) === RANDOM37_B32, "Base32Encode: 37 random bytes vs python oracle");
    assert(eqArr(Base32Decode(RANDOM37_B32), RANDOM37), "Base32Decode: 37 random bytes vs python oracle");
    assert(Base32HexEncode(RANDOM37) === RANDOM37_B32HEX, "Base32HexEncode: 37 random bytes vs python oracle");
    assert(eqArr(Base32HexDecode(RANDOM37_B32HEX), RANDOM37), "Base32HexDecode: 37 random bytes vs python oracle");

    assertThrows(() => Base32Decode("my======"), "Base32Decode: lowercase rejected", SyntaxError);
    assertThrows(() => Base32Decode("0Y======"), "Base32Decode: digit '0' invalid in standard alphabet", SyntaxError);
    assertThrows(() => Base32HexDecode("WO======"), "Base32HexDecode: letter 'W' invalid in hex alphabet", SyntaxError);
    assertThrows(() => Base32Decode("MY====="), "Base32Decode: length not a multiple of 8", SyntaxError);
    assertThrows(() => Base32Decode("MY======MZXW6YTB"), "Base32Decode: padding before the final block rejected", SyntaxError);

    for (let len = 0; len <= 64; len++) {
        const b = randBytes(len);
        assert(eqArr(Base32Decode(Base32Encode(b)), b), "base32 roundtrip fuzz len=" + len);
        assert(eqArr(Base32HexDecode(Base32HexEncode(b)), b), "base32hex roundtrip fuzz len=" + len);
    }
}

{
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
}

{
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
}

{
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
}

{
    assert(eqArr(PutUvarint({ valueOf() { return 300; } }), u8(0xAC, 0x02)),
        "PutUvarint: accepts a valueOf-coercible object");
    assert(eqArr(PutVarint({ valueOf() { return -1; } }), u8(1)),
        "PutVarint: accepts a valueOf-coercible object");
    assertThrows(() => HexEncode(new String("abc")),
        "HexEncode: String wrapper object (boxed) is rejected, not silently unboxed", TypeError);
}

{
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
}

{
    assertThrows(() => Base85Decode("s8"), "a85 overlong partial group refused", SyntaxError);
    assertThrows(() => Base85Decode("uu"), "a85 overlong partial (84*85^4 > 2^32-1) refused", SyntaxError);
    eqArr(Array.from(Base85Decode("s8N")), [255, 255], "a85 partial at the 2^32-1 boundary still decodes");
}

print("test_encoding: all tests passed (" + n + " assertions)");
