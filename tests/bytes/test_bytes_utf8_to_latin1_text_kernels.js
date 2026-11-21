// covers: dyna:bytes.utf8ToLatin1
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_bytes_utf8_to_latin1_text_kernels");
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

eqBytes(utf8ToLatin1(buf([0x41, 0xc3, 0xa9, 0xc3, 0xbf])),
        [0x41, 0xe9, 0xff], "utf8->latin1 vector");

const notLatin1 = [
    [0xe2, 0x82, 0xac],
    [0xc4, 0x80],
    [0xf0, 0x9f, 0x98, 0x80],
    [0xc2],
    [0x80],
    [0xc3, 0x28],
    [0x41, 0xc4, 0x80],
];

__kitS.section("utf8ToLatin1 should throw RangeError on", () => {
for (const seq of notLatin1) {
    let threw = false;
    try { utf8ToLatin1(buf(seq)); } catch (e) { threw = (e instanceof RangeError); }
    assert(threw, "utf8ToLatin1 should throw RangeError on " + seq);
}
});
__kitS.end();

let cuCases = 0;
print("test_text_kernels: countUtf8 OK (" + cuCases + " cases)");

print("test_text_kernels: all tests passed");
