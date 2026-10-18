import { latin1ToUtf8, utf8ToLatin1, countUtf8 } from "dyna:bytes";
import { HexEncode, HexDecode } from "dyna:encoding";

function assert(c, m) { if (!c) throw new Error("assertion failed: " + m); }

let seed = 0x9e3779b9 >>> 0;
function rnd() { seed = (seed * 1664525 + 1013904223) >>> 0; return seed; }
function buf(arr) { return new Uint8Array(arr).buffer; }
function eqBytes(a, b, m) {
    assert(a.length === b.length, m + " length " + a.length + " vs " + b.length);
    for (let i = 0; i < a.length; i++) assert(a[i] === b[i], m + " byte " + i);
}
const HEX = "0123456789abcdef";
function hexRef(bytes) {
    let s = "";
    for (let i = 0; i < bytes.length; i++)
        s += HEX[bytes[i] >> 4] + HEX[bytes[i] & 0xF];
    return s;
}

let cases = 0;

assert(HexEncode(buf([])) === "", "hex empty");
assert(HexEncode(buf([0xde, 0xad, 0xbe, 0xef])) === "deadbeef", "hex deadbeef");
assert(HexEncode(buf([0x00, 0x0f, 0xf0, 0xff])) === "000ff0ff", "hex nibbles");
assert(HexEncode("A") === "41", "HexEncode string -> utf8 bytes");

for (let len = 0; len <= 200; len++) {
    const bytes = [];
    for (let i = 0; i < len; i++) bytes.push(rnd() & 0xFF);
    const enc = HexEncode(buf(bytes));
    assert(enc === hexRef(bytes), "HexEncode mismatch len=" + len);
    const dec = HexDecode(enc);
    assert(dec instanceof Uint8Array, "HexDecode returns Uint8Array");
    eqBytes(dec, bytes, "hex roundtrip len=" + len);
    eqBytes(HexDecode(enc.toUpperCase()), bytes, "hex UPPER roundtrip len=" + len);
    cases++;
}
for (const bad of ["a", "abc", "gg", "0x", "zz", "12 34", "деад"]) {
    let threw = false;
    try { HexDecode(bad); } catch (e) { threw = true; }
    assert(threw, "HexDecode should reject " + JSON.stringify(bad));
}
assert(HexDecode("").length === 0, "HexDecode empty -> empty");
print("test_text_kernels: hex OK (" + cases + " roundtrip lengths)");

function l2uRef(bytes) {
    const o = [];
    for (const c of bytes) {
        if (c < 0x80) o.push(c);
        else { o.push(0xC0 | (c >> 6)); o.push(0x80 | (c & 0x3F)); }
    }
    return o;
}
eqBytes(latin1ToUtf8(buf([0x41, 0xe9, 0xff])),
        [0x41, 0xc3, 0xa9, 0xc3, 0xbf], "latin1->utf8 vector");
eqBytes(utf8ToLatin1(buf([0x41, 0xc3, 0xa9, 0xc3, 0xbf])),
        [0x41, 0xe9, 0xff], "utf8->latin1 vector");

let l1cases = 0;
for (let len = 0; len <= 200; len++) {
    for (let mode = 0; mode < 3; mode++) {
        const bytes = [];
        for (let i = 0; i < len; i++) {
            const r = rnd();
            bytes.push(mode === 0 ? (r & 0xFF)
                     : mode === 1 ? (r & 0x7F)
                     : (0x80 | (r & 0x7F)));
        }
        const u = latin1ToUtf8(buf(bytes));
        eqBytes(u, l2uRef(bytes), "latin1->utf8 len=" + len + " mode=" + mode);
        const back = utf8ToLatin1(u.buffer);
        eqBytes(back, bytes, "latin1 roundtrip len=" + len + " mode=" + mode);
        l1cases++;
    }
}
const notLatin1 = [
    [0xe2, 0x82, 0xac],
    [0xc4, 0x80],
    [0xf0, 0x9f, 0x98, 0x80],
    [0xc2],
    [0x80],
    [0xc3, 0x28],
    [0x41, 0xc4, 0x80],
];
for (const seq of notLatin1) {
    let threw = false;
    try { utf8ToLatin1(buf(seq)); } catch (e) { threw = (e instanceof RangeError); }
    assert(threw, "utf8ToLatin1 should throw RangeError on " + seq);
}
print("test_text_kernels: latin1<->utf8 OK (" + l1cases + " roundtrips)");

function cpBytes(cp) {
    if (cp < 0x80) return [cp];
    if (cp < 0x800) return [0xC0 | (cp >> 6), 0x80 | (cp & 0x3F)];
    if (cp < 0x10000)
        return [0xE0 | (cp >> 12), 0x80 | ((cp >> 6) & 0x3F), 0x80 | (cp & 0x3F)];
    return [0xF0 | (cp >> 18), 0x80 | ((cp >> 12) & 0x3F),
            0x80 | ((cp >> 6) & 0x3F), 0x80 | (cp & 0x3F)];
}
let cuCases = 0;
for (let rep = 0; rep < 4000; rep++) {
    const bytes = [];
    const ncp = rnd() % 60;
    for (let i = 0; i < ncp; i++) {
        let cp;
        do { cp = rnd() % 0x110000; } while (cp >= 0xD800 && cp <= 0xDFFF);
        bytes.push(...cpBytes(cp));
    }
    assert(countUtf8(buf(bytes)) === ncp, "countUtf8 mismatch @rep" + rep);
    cuCases++;
}
assert(countUtf8("héllo") === 5, "countUtf8 string héllo");
assert(countUtf8("") === 0, "countUtf8 empty");
assert(countUtf8("a".repeat(100)) === 100, "countUtf8 ascii");
print("test_text_kernels: countUtf8 OK (" + cuCases + " cases)");

print("test_text_kernels: all tests passed");
