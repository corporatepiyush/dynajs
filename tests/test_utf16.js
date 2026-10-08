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

eqBytes(utf16ToUtf8(u16le(0x41, 0x42)), u8([0x41, 0x42]), "utf16ToUtf8 AB");
eqBytes(utf16ToUtf8(u16le(0x20AC)), u8([0xE2, 0x82, 0xAC]), "utf16ToUtf8 euro");
eqBytes(utf16ToUtf8(u16le(0xD83D, 0xDE00)), u8([0xF0, 0x9F, 0x98, 0x80]), "utf16ToUtf8 emoji");
eqBytes(utf16ToUtf8(u8([])), u8([]), "utf16ToUtf8 empty");
assert(utf8ToUtf16("A") instanceof Uint8Array, "utf8ToUtf16 returns Uint8Array");
assert(utf16ToUtf8(u16le(0x41)) instanceof Uint8Array, "utf16ToUtf8 returns Uint8Array");
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
{
    const s = "😀".repeat(200);
    const u16 = utf8ToUtf16(s);
    eqBytes(u16, strToUtf16le(s), "emoji run bytes");
    eqBytes(utf16ToUtf8(u16), strToUtf8(s), "emoji run roundtrip");
    assert(countUtf16(u16) === 200, "emoji run countUtf16");
    assert(isValidUtf16(u16), "emoji run valid");
}
print("test_utf16: fast paths OK (" + fastCases + " lengths)");

assert(isValidUtf16(u16le(0x41, 0x42)) === true, "valid ascii");
assert(isValidUtf16(u16le(0xD83D, 0xDE00)) === true, "valid pair");
assert(isValidUtf16(u16le(0xFEFF)) === true, "valid BOM");
assert(isValidUtf16(u8([])) === true, "valid empty");
assert(isValidUtf16(u16le(0x41, 0xD800, 0xDC00, 0x42)) === true, "valid pair in middle");

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
for (const [name, bytes] of Object.entries(bad16)) {
    assert(isValidUtf16(bytes) === false, "isValidUtf16 should reject: " + name);
    let threw = false;
    try { utf16ToUtf8(bytes); } catch (e) { threw = (e instanceof RangeError); }
    assert(threw, "utf16ToUtf8 should throw RangeError on: " + name);
}
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
for (const seq of badUtf8) {
    let threw = false;
    try { utf8ToUtf16(u8(seq)); } catch (e) { threw = (e instanceof RangeError); }
    assert(threw, "utf8ToUtf16 should throw RangeError on " + JSON.stringify(seq));
}
print("test_utf16: malformed-UTF-8 reject OK");

assert(countUtf16(u16le(0x41, 0x42, 0x43)) === 3, "countUtf16 ascii");
assert(countUtf16(u16le(0xD83D, 0xDE00)) === 1, "countUtf16 one emoji");
assert(countUtf16(u16le(0x41, 0xD83D, 0xDE00, 0x42)) === 3, "countUtf16 mixed");
assert(countUtf16(u8([])) === 0, "countUtf16 empty");
{
    let threw = false;
    try { countUtf16(u8([0x41, 0x00, 0x42])); }
    catch (e) { threw = true; }
    assert(threw, "countUtf16 odd byte length throws");
}
print("test_utf16: countUtf16 OK");

print("test_utf16: all tests passed (" + (cases + fastCases) + " sweep cases)");
