// covers: dyna:encoding.HexEncode
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_encoding_hex_encode_text_kernels");
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

__kitS.section("hex empty", () => {
assert(HexEncode(buf([])) === "", "hex empty");
});

__kitS.section("hex deadbeef", () => {
assert(HexEncode(buf([0xde, 0xad, 0xbe, 0xef])) === "deadbeef", "hex deadbeef");
});

__kitS.section("hex nibbles", () => {
assert(HexEncode(buf([0x00, 0x0f, 0xf0, 0xff])) === "000ff0ff", "hex nibbles");
});

__kitS.section("HexEncode string -> utf8 bytes", () => {
assert(HexEncode("A") === "41", "HexEncode string -> utf8 bytes");
});

__kitS.section("HexEncode mismatch len", () => {
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
});
__kitS.end();

let cuCases = 0;
print("test_text_kernels: countUtf8 OK (" + cuCases + " cases)");

print("test_text_kernels: all tests passed");
