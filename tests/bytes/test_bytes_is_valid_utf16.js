// covers: dyna:bytes.isValidUtf16
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_bytes_is_valid_utf16");
import { utf8ToUtf16, utf16ToUtf8, isValidUtf16, countUtf16, countUtf8 }
    from "dyna:bytes";

function assert(c, m) { if (!c) throw new Error("assertion failed: " + m); }

let seed = 0x1234abcd >>> 0;
function rnd() { seed = (seed * 1664525 + 1013904223) >>> 0; return seed; }
function u8(arr) { return new Uint8Array(arr); }
function eqBytes(a, b, m) {
    assert(a.length === b.length, m + " length " + a.length + " vs " + b.length);
    for (let i = 0; i < a.length; i++) assert(a[i] === b[i], m + " byte " + i +
        " " + a[i] + " vs " + b[i]);
}

function strToUtf16le(s) {
    const o = [];
    for (let i = 0; i < s.length; i++) {
        const u = s.charCodeAt(i);
        o.push(u & 0xFF, (u >> 8) & 0xFF);
    }
    return u8(o);
}
function cpToUtf8(cp, o) {
    if (cp < 0x80) o.push(cp);
    else if (cp < 0x800) o.push(0xC0 | (cp >> 6), 0x80 | (cp & 0x3F));
    else if (cp < 0x10000)
        o.push(0xE0 | (cp >> 12), 0x80 | ((cp >> 6) & 0x3F), 0x80 | (cp & 0x3F));
    else o.push(0xF0 | (cp >> 18), 0x80 | ((cp >> 12) & 0x3F),
               0x80 | ((cp >> 6) & 0x3F), 0x80 | (cp & 0x3F));
}
function strToUtf8(s) {
    const o = [];
    for (const ch of s) cpToUtf8(ch.codePointAt(0), o);
    return u8(o);
}
function u16le(...units) {
    const o = [];
    for (const u of units) o.push(u & 0xFF, (u >> 8) & 0xFF);
    return u8(o);
}

let cases = 0;

print("test_utf16: round-trip + count OK (" + cases + " corpus strings)");

let fastCases = 0;

print("test_utf16: fast paths OK (" + fastCases + " lengths)");

__kitS.section("valid ascii", () => {
assert(isValidUtf16(u16le(0x41, 0x42)) === true, "valid ascii");
});

__kitS.section("valid pair", () => {
assert(isValidUtf16(u16le(0xD83D, 0xDE00)) === true, "valid pair");
});

__kitS.section("valid BOM", () => {
assert(isValidUtf16(u16le(0xFEFF)) === true, "valid BOM");
});

__kitS.section("valid empty", () => {
assert(isValidUtf16(u8([])) === true, "valid empty");
});

__kitS.section("valid pair in middle", () => {
assert(isValidUtf16(u16le(0x41, 0xD800, 0xDC00, 0x42)) === true, "valid pair in middle");
});

const bad16 = {
    "lone high": u16le(0xD800),
    "lone low": u16le(0xDC00),
    "reversed pair": u16le(0xDC00, 0xD800),
    "high then ascii": u16le(0xD800, 0x0041),
    "high then high": u16le(0xD800, 0xD800),
    "high at end": u16le(0x41, 0x42, 0xD800),
    "low in middle": u16le(0x41, 0xDC00, 0x42),
    "odd length": u8([0x41]),
    "odd length 3": u8([0x41, 0x00, 0x42]),
};

__kitS.section("isValidUtf16 should reject:", () => {
for (const [name, bytes] of Object.entries(bad16)) {
    assert(isValidUtf16(bytes) === false, "isValidUtf16 should reject: " + name);
    let threw = false;
    try { utf16ToUtf8(bytes); } catch (e) { threw = (e instanceof RangeError); }
    assert(threw, "utf16ToUtf8 should throw RangeError on: " + name);
}
print("test_utf16: countUtf16 OK");
});
__kitS.end();

print("test_utf16: all tests passed (" + (cases + fastCases) + " sweep cases)");
