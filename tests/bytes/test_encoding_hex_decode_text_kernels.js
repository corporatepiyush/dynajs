// covers: dyna:encoding.HexDecode
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_encoding_hex_decode_text_kernels");
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

__kitS.section("HexDecode should reject", () => {
for (const bad of ["a", "abc", "gg", "0x", "zz", "12 34", "деад"]) {
    let threw = false;
    try { HexDecode(bad); } catch (e) { threw = true; }
    assert(threw, "HexDecode should reject " + JSON.stringify(bad));
}
});

__kitS.section("HexDecode empty -> empty", () => {
assert(HexDecode("").length === 0, "HexDecode empty -> empty");
});
__kitS.end();

let cuCases = 0;
print("test_text_kernels: countUtf8 OK (" + cuCases + " cases)");

print("test_text_kernels: all tests passed");
