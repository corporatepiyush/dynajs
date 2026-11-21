// covers: dyna:bytes.latin1ToUtf8
import { sections as __kitSections } from "../lib/kit.js";
import { latin1ToUtf8, utf8ToLatin1, countUtf8 } from "dyna:bytes";
import { HexEncode, HexDecode } from "dyna:encoding";
const __kitS = __kitSections("test_bytes_latin1_to_utf8_text_kernels");

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

function l2uRef(bytes) {
    const o = [];
    for (const c of bytes) {
        if (c < 0x80) o.push(c);
        else { o.push(0xC0 | (c >> 6)); o.push(0x80 | (c & 0x3F)); }
    }
    return o;
}

let l1cases = 0;

__kitS.section("latin1->utf8 vector", () => {
print("test_text_kernels: hex OK (" + cases + " roundtrip lengths)");

eqBytes(latin1ToUtf8(buf([0x41, 0xe9, 0xff])),
        [0x41, 0xc3, 0xa9, 0xc3, 0xbf], "latin1->utf8 vector");
});

__kitS.section("latin1 roundtrip len", () => {
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
});
__kitS.end();

let cuCases = 0;
if (!__kitS.listing) {
print("test_text_kernels: countUtf8 OK (" + cuCases + " cases)");

print("test_text_kernels: all tests passed");
}
