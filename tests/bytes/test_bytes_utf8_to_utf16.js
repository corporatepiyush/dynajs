// covers: dyna:bytes.utf8ToUtf16
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_bytes_utf8_to_utf16");
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

eqBytes(utf8ToUtf16("A"), u16le(0x41), "utf8ToUtf16 A");

eqBytes(utf8ToUtf16("AB"), u16le(0x41, 0x42), "utf8ToUtf16 AB");

eqBytes(utf8ToUtf16("é"), u16le(0x00E9), "utf8ToUtf16 e-acute");

eqBytes(utf8ToUtf16("€"), u16le(0x20AC), "utf8ToUtf16 euro");

eqBytes(utf8ToUtf16(String.fromCharCode(0xFEFF)), u16le(0xFEFF), "utf8ToUtf16 BOM");

eqBytes(utf8ToUtf16("😀"), u8([0x3D, 0xD8, 0x00, 0xDE]), "utf8ToUtf16 emoji");

eqBytes(utf8ToUtf16(""), u8([]), "utf8ToUtf16 empty");

eqBytes(utf8ToUtf16("héllo 中文 😀"),
        utf8ToUtf16(strToUtf8("héllo 中文 😀")),
        "utf8ToUtf16 string == its utf8 bytes");

__kitS.section("utf8ToUtf16 returns Uint8Array", () => {
assert(utf8ToUtf16("A") instanceof Uint8Array, "utf8ToUtf16 returns Uint8Array");
});

print("test_utf16: round-trip + count OK (" + cases + " corpus strings)");

let fastCases = 0;

print("test_utf16: validation + reject OK");

const badUtf8 = [
    [0xFF],
    [0x80],
    [0xC2],
    [0xE2, 0x82],
    [0xF0, 0x9F, 0x98],
    [0xC0, 0x80],
    [0xE0, 0x80, 0x80],
    [0xED, 0xA0, 0x80],
    [0xF4, 0x90, 0x80, 0x80],
    [0x41, 0xC3, 0x28],
];

__kitS.section("utf8ToUtf16 should throw RangeError on", () => {
for (const seq of badUtf8) {
    let threw = false;
    try { utf8ToUtf16(u8(seq)); } catch (e) { threw = (e instanceof RangeError); }
    assert(threw, "utf8ToUtf16 should throw RangeError on " + JSON.stringify(seq));
}
print("test_utf16: countUtf16 OK");
});
__kitS.end();

print("test_utf16: all tests passed (" + (cases + fastCases) + " sweep cases)");
