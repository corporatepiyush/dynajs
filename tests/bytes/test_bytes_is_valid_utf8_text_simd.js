// covers: dyna:bytes.isValidUtf8
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_bytes_is_valid_utf8_text_simd");
import { Bytes, isValidUtf8 }
    from "dyna:bytes";

function assert(c, m) { if (!c) throw new Error("assertion failed: " + m); }

let seed = 0x12345678 >>> 0;
function rnd() { seed = (seed * 1664525 + 1013904223) >>> 0; return seed; }
const AB = "abcd";
function randAscii(len) {
    let s = "";
    for (let i = 0; i < len; i++) s += AB[rnd() % AB.length];
    return s;
}

let cases = 0;

print("test_text_simd: " + cases + " count/find_first_of cases OK");

function utf8Bytes(cp) {
    if (cp < 0x80) return [cp];
    if (cp < 0x800) return [0xC0 | (cp >> 6), 0x80 | (cp & 0x3F)];
    if (cp < 0x10000)
        return [0xE0 | (cp >> 12), 0x80 | ((cp >> 6) & 0x3F), 0x80 | (cp & 0x3F)];
    return [0xF0 | (cp >> 18), 0x80 | ((cp >> 12) & 0x3F),
            0x80 | ((cp >> 6) & 0x3F), 0x80 | (cp & 0x3F)];
}

function buf(arr) { return new Uint8Array(arr).buffer; }

__kitS.section("valid utf8 rejected @rep", () => {
for (let rep = 0; rep < 3000; rep++) {
    const bytes = [];
    const ncp = rnd() % 40;
    for (let i = 0; i < ncp; i++) {
        let cp;
        do { cp = rnd() % 0x110000; } while (cp >= 0xD800 && cp <= 0xDFFF);
        bytes.push(...utf8Bytes(cp));
    }
    assert(isValidUtf8(buf(bytes)) === true, "valid utf8 rejected @rep" + rep);
}
});

const invalid = [
    [0x80],
    [0xC0, 0x80],
    [0xE0, 0x80, 0x80],
    [0xF0, 0x80, 0x80, 0x80],
    [0xED, 0xA0, 0x80],
    [0xC2],
    [0xE2, 0x82],
    [0xF4, 0x90, 0x80, 0x80],
    [0x41, 0xFF, 0x42],
    [0xC2, 0x41],
];

__kitS.section("invalid utf8 accepted:", () => {
for (const seq of invalid)
    assert(isValidUtf8(buf(seq)) === false,
           "invalid utf8 accepted: " + seq);
});

__kitS.section("bad byte @", () => {
for (let off = 0; off < 40; off++) {
    const b = [];
    for (let i = 0; i < 40; i++) b.push(i === off ? 0xFF : 0x61);
    assert(isValidUtf8(buf(b)) === false, "bad byte @" + off + " accepted");
}
print("test_text_simd: utf8 validation OK");
});
__kitS.end();

print("test_text_simd: all tests passed");
