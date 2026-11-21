// covers: dyna:bytes.encode
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_bytes_encode_iconv");
import { decode, encode, encodingExists, encodings } from "dyna:bytes";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function eq(a, b, msg) {
    assert(a === b, msg + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
}
function throws(fn, msg) {
    let t = false;
    try { fn(); } catch (e) { t = true; }
    assert(t, msg);
}
const B = (...xs) => new Uint8Array(xs);

__kitS.section("EURO encodes to 0x80 in cp1252", () => {
eq(Array.from(encode("\u20AC", "windows-1252")).join(","), "128",
   "EURO encodes to 0x80 in cp1252");
});

__kitS.section("e-acute encodes to 0xE9 in latin-1", () => {
eq(Array.from(encode("\u00E9", "iso-8859-1")).join(","), "233",
   "e-acute encodes to 0xE9 in latin-1");
});

__kitS.section("ASCII encodes as itself", () => {
eq(Array.from(encode("abc", "windows-1252")).join(","), "97,98,99", "ASCII encodes as itself");
});

__kitS.section("empty string encodes to empty", () => {
eq(encode("", "windows-1252").length, 0, "empty string encodes to empty");
});

__kitS.section("an inexpressible code point becomes '?'", () => {
eq(Array.from(encode("\u4F60", "iso-8859-1")).join(","), "63",
   "an inexpressible code point becomes '?'");
});

__kitS.section("substitution happens in place", () => {
eq(Array.from(encode("a\u4F60b", "windows-1252")).join(","), "97,63,98",
   "substitution happens in place");
});

__kitS.section("an astral character substitutes as a single byte", () => {
eq(encode("\u{1f600}", "windows-1252").length, 1,
   "an astral character substitutes as a single byte");
});

__kitS.section("encode refuses an unknown label", () => {
throws(() => encode("a", "no-such-encoding"), "encode refuses an unknown label");
});

__kitS.section("encode refuses a non-string input", () => {
throws(() => encode(42, "windows-1252"), "encode refuses a non-string input");
});
__kitS.end();

if (fails) {
    print("test_iconv: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_iconv failed");
}
print("test_iconv: " + n + " assertions, 0 failures");
