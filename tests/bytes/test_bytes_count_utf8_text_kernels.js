// covers: dyna:bytes.countUtf8
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_bytes_count_utf8_text_kernels");
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

let l1cases = 0;

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

__kitS.section("countUtf8 mismatch @rep", () => {
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
});

__kitS.section("countUtf8 string héllo", () => {
assert(countUtf8("héllo") === 5, "countUtf8 string héllo");
});

__kitS.section("countUtf8 empty", () => {
assert(countUtf8("") === 0, "countUtf8 empty");
});

__kitS.section("countUtf8 ascii", () => {
assert(countUtf8("a".repeat(100)) === 100, "countUtf8 ascii");
print("test_text_kernels: countUtf8 OK (" + cuCases + " cases)");
});
__kitS.end();

print("test_text_kernels: all tests passed");
