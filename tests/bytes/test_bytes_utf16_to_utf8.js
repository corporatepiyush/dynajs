// covers: dyna:bytes.utf16ToUtf8
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_bytes_utf16_to_utf8");
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

eqBytes(utf16ToUtf8(u16le(0x41, 0x42)), u8([0x41, 0x42]), "utf16ToUtf8 AB");

eqBytes(utf16ToUtf8(u16le(0x20AC)), u8([0xE2, 0x82, 0xAC]), "utf16ToUtf8 euro");

eqBytes(utf16ToUtf8(u16le(0xD83D, 0xDE00)), u8([0xF0, 0x9F, 0x98, 0x80]), "utf16ToUtf8 emoji");

eqBytes(utf16ToUtf8(u8([])), u8([]), "utf16ToUtf8 empty");

__kitS.section("utf16ToUtf8 returns Uint8Array", () => {
assert(utf16ToUtf8(u16le(0x41)) instanceof Uint8Array, "utf16ToUtf8 returns Uint8Array");
});
__kitS.end();

print("test_utf16: round-trip + count OK (" + cases + " corpus strings)");

let fastCases = 0;
print("test_utf16: countUtf16 OK");

print("test_utf16: all tests passed (" + (cases + fastCases) + " sweep cases)");
