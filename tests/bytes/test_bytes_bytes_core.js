// covers: dyna:bytes.Bytes
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_bytes_bytes_core");
import { Bytes, compare, equal, indexOf, lastIndexOf, contains, count, concat, copy, isValidUtf8, countUtf8, utf8ToLatin1, countUtf16 } from "dyna:bytes";
import { HexEncode as toHex, HexDecode as fromHex, Base64Encode as toBase64, Base64Decode as fromBase64 } from "dyna:encoding";

const fromUtf8 = (str) => new TextEncoder().encode(str);
const toUtf8 = (v) => new TextDecoder().decode(v);


let n = 0;
function assert(cond, msg) {
    n++;
    if (!cond) throw new Error("assertion failed: " + msg);
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

print("test_bytes: all tests passed (" + n + " assertions)");

__kitS.section("toUtf8 lossy:", () => {
    const expect = (bytes, want, what) => {
        assert(toUtf8(new Uint8Array(bytes)) === want,
            "toUtf8 lossy: " + what, JSON.stringify(toUtf8(new Uint8Array(bytes))));
        assert(new Bytes(new Uint8Array(bytes)).toString() === want,
            "Bytes.toString lossy: " + what);
    };
    expect([0x61, 0xC3, 0x28], "a\uFFFD(", "truncated 2-byte at ASCII");
    expect([0xED, 0xA0, 0x80], "\uFFFD\uFFFD\uFFFD", "WTF-8 surrogate -> 3 x U+FFFD, not a lone surrogate");
    expect([0xC0, 0xAF], "\uFFFD\uFFFD", "overlong 2-byte -> 2 x U+FFFD");
    expect([0xF4, 0x90, 0x80, 0x80], "\uFFFD\uFFFD\uFFFD\uFFFD", "out of range -> 4 x U+FFFD");
    expect([0xE2, 0x82, 0x41], "\uFFFDA", "bad continuation consumed as one subpart");
    expect([0xF0, 0x9F, 0x98, 0x80], "\u{1F600}", "valid 4-byte unchanged");
    assert(toUtf8(new Uint8Array()) === "", "toUtf8 lossy: empty stays empty");
});

__kitS.section("hello world", () => {
    const b = new Bytes("hello world");
    assert(b.slice(100).length === 0, "slice(100) past end -> empty");
    assert(b.slice(100, 105).length === 0, "slice(100,105) past end -> empty");
    assert(b.slice(11).length === 0, "slice(len) -> empty");
    assert(b.slice(5).toString() === " world", "slice(5) in range unchanged");
    assert(b.slice(-100).toString() === "hello world", "slice(-100) clamps low");
});

__kitS.section("alloc(2^31) throws the module error", () => {
    assertThrows(() => Bytes.alloc(2 ** 31), "alloc(2^31) throws the module error", RangeError);
    assertThrows(() => Bytes.alloc(2 ** 32), "alloc(2^32) throws", RangeError);
    assert(Bytes.alloc(0).length === 0, "alloc(0) still works");
});
__kitS.end();
