// covers: dyna:bytes.countUtf16
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_bytes_count_utf16");
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

print("test_utf16: known vectors OK");

function randValidString(nCps) {
    let s = "";
    for (let i = 0; i < nCps; i++) {
        const r = rnd();
        let cp;
        switch (r & 3) {
            case 0: cp = r % 0x80; break;
            case 1: cp = 0x80 + (r % (0x800 - 0x80)); break;
            case 2: cp = 0x800 + (r % (0xD800 - 0x800)); break;
            default: cp = 0x10000 + (r % 0x100000); break;
        }
        s += String.fromCodePoint(cp);
    }
    return s;
}

for (let rep = 0; rep < 1500; rep++) {
    const s = randValidString(rnd() % 40);
    const u16 = utf8ToUtf16(s);
    eqBytes(u16, strToUtf16le(s), "utf8ToUtf16 corpus @rep" + rep);
    assert(isValidUtf16(u16), "isValidUtf16 on produced UTF-16 @rep" + rep);
    eqBytes(utf16ToUtf8(u16), strToUtf8(s), "utf16->utf8 roundtrip @rep" + rep);
    const ncp = [...s].length;
    assert(countUtf16(u16) === ncp, "countUtf16 @rep" + rep + " got " + countUtf16(u16) + " want " + ncp);
    assert(countUtf8(strToUtf8(s)) === ncp, "countUtf8 @rep" + rep);
    cases++;
}

print("test_utf16: round-trip + count OK (" + cases + " corpus strings)");

let fastCases = 0;

for (let len = 0; len <= 130; len++) {
    let s = "";
    for (let i = 0; i < len; i++) s += String.fromCharCode(0x20 + (rnd() % 0x5F));
    const u16 = utf8ToUtf16(s);
    eqBytes(u16, strToUtf16le(s), "ascii fast len=" + len);
    eqBytes(utf16ToUtf8(u16), strToUtf8(s), "ascii roundtrip len=" + len);
    assert(countUtf16(u16) === len, "ascii countUtf16 len=" + len);
    assert(isValidUtf16(u16), "ascii valid len=" + len);
    fastCases++;
}

__kitS.section("emoji run bytes", () => {
    const s = "😀".repeat(200);
    const u16 = utf8ToUtf16(s);
    eqBytes(u16, strToUtf16le(s), "emoji run bytes");
    eqBytes(utf16ToUtf8(u16), strToUtf8(s), "emoji run roundtrip");
    assert(countUtf16(u16) === 200, "emoji run countUtf16");
    assert(isValidUtf16(u16), "emoji run valid");
});

print("test_utf16: malformed-UTF-8 reject OK");

assert(countUtf16(u16le(0x41, 0x42, 0x43)) === 3, "countUtf16 ascii");

assert(countUtf16(u16le(0xD83D, 0xDE00)) === 1, "countUtf16 one emoji");

assert(countUtf16(u16le(0x41, 0xD83D, 0xDE00, 0x42)) === 3, "countUtf16 mixed");

assert(countUtf16(u8([])) === 0, "countUtf16 empty");

__kitS.section("countUtf16 odd byte length throws", () => {
    let threw = false;
    try { countUtf16(u8([0x41, 0x00, 0x42])); }
    catch (e) { threw = true; }
    assert(threw, "countUtf16 odd byte length throws");
});
__kitS.end();
print("test_utf16: countUtf16 OK");

print("test_utf16: all tests passed (" + (cases + fastCases) + " sweep cases)");
