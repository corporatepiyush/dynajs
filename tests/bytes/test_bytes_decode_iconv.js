// covers: dyna:bytes.decode
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_bytes_decode_iconv");
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

__kitS.section("cp1252 0x80 is EURO SIGN", () => {
eq(decode(B(0x80), "windows-1252"), "\u20AC", "cp1252 0x80 is EURO SIGN");
});

__kitS.section("cp1252 0x82 is SINGLE LOW-9 QUOTE", () => {
eq(decode(B(0x82), "windows-1252"), "\u201A", "cp1252 0x82 is SINGLE LOW-9 QUOTE");
});

__kitS.section("cp1252 0x85 is HORIZONTAL ELLIPSIS", () => {
eq(decode(B(0x85), "windows-1252"), "\u2026", "cp1252 0x85 is HORIZONTAL ELLIPSIS");
});

__kitS.section("cp1252 0x91 is LEFT SINGLE QUOTE", () => {
eq(decode(B(0x91), "windows-1252"), "\u2018", "cp1252 0x91 is LEFT SINGLE QUOTE");
});

__kitS.section("cp1252 0x92 is RIGHT SINGLE QUOTE", () => {
eq(decode(B(0x92), "windows-1252"), "\u2019", "cp1252 0x92 is RIGHT SINGLE QUOTE");
});

__kitS.section("cp1252 0x93 is LEFT DOUBLE QUOTE", () => {
eq(decode(B(0x93), "windows-1252"), "\u201C", "cp1252 0x93 is LEFT DOUBLE QUOTE");
});

__kitS.section("cp1252 0x99 is TRADE MARK SIGN", () => {
eq(decode(B(0x99), "windows-1252"), "\u2122", "cp1252 0x99 is TRADE MARK SIGN");
});

__kitS.section("cp1252 0xE9 is e-acute", () => {
eq(decode(B(0xE9), "windows-1252"), "\u00E9", "cp1252 0xE9 is e-acute");
});

__kitS.section("latin-1 0x80 is U+0080, not EURO", () => {
eq(decode(B(0x80), "iso-8859-1"), "\u0080", "latin-1 0x80 is U+0080, not EURO");
});

__kitS.section("latin-1 0xE9 is e-acute", () => {
eq(decode(B(0xE9), "iso-8859-1"), "\u00E9", "latin-1 0xE9 is e-acute");
});

__kitS.section("latin-1 and cp1252 really do differ at 0x80", () => {
assert(decode(B(0x80), "iso-8859-1") !== decode(B(0x80), "windows-1252"),
       "latin-1 and cp1252 really do differ at 0x80");
});

__kitS.section("iso-8859-15 0xA4 is EURO SIGN", () => {
eq(decode(B(0xA4), "iso-8859-15"), "\u20AC", "iso-8859-15 0xA4 is EURO SIGN");
});

__kitS.section("iso-8859-1 0xA4 is CURRENCY SIGN", () => {
eq(decode(B(0xA4), "iso-8859-1"), "\u00A4", "iso-8859-1 0xA4 is CURRENCY SIGN");
});

__kitS.section("cp1251 0xC0 is CYRILLIC A", () => {
eq(decode(B(0xC0), "windows-1251"), "\u0410", "cp1251 0xC0 is CYRILLIC A");
});

__kitS.section("koi8-r 0xC0 is lowercase yu", () => {
eq(decode(B(0xC0), "koi8-r"), "\u044E", "koi8-r 0xC0 is lowercase yu");
});

__kitS.section("koi8-r 0xE0 is UPPERCASE Yu", () => {
eq(decode(B(0xE0), "koi8-r"), "\u042E", "koi8-r 0xE0 is UPPERCASE Yu");
});

__kitS.section("ibm866 0x80 is CYRILLIC A", () => {
eq(decode(B(0x80), "ibm866"), "\u0410", "ibm866 0x80 is CYRILLIC A");
});

__kitS.section("iso-8859-7 0xE1 is GREEK ALPHA", () => {
eq(decode(B(0xE1), "iso-8859-7"), "\u03B1", "iso-8859-7 0xE1 is GREEK ALPHA");
});

__kitS.section("iso-8859-8 0xE0 is HEBREW ALEF", () => {
eq(decode(B(0xE0), "iso-8859-8"), "\u05D0", "iso-8859-8 0xE0 is HEBREW ALEF");
});

__kitS.section("macintosh 0xA5 is BULLET", () => {
eq(decode(B(0xA5), "macintosh"), "\u2022", "macintosh 0xA5 is BULLET");
});

__kitS.section("the low half is identity in", () => {
for (const label of ["windows-1252", "iso-8859-1", "koi8-r", "iso-8859-7", "macintosh"]) {
    const bytes = [];
    for (let b = 0; b < 0x80; b++) bytes.push(b);
    let want = "";
    for (let b = 0; b < 0x80; b++) want += String.fromCharCode(b);
    eq(decode(new Uint8Array(bytes), label), want,
       "the low half is identity in " + label);
}
});

__kitS.section("iso-8859-3 0xA1 is H-stroke", () => {
eq(decode(B(0xA1), "iso-8859-3"), "\u0126", "iso-8859-3 0xA1 is H-stroke");
});

__kitS.section("an undefined byte decodes to U+FFFD", () => {
eq(decode(B(0xA5), "iso-8859-3"), "\uFFFD", "an undefined byte decodes to U+FFFD");
});

__kitS.section("us-ascii rejects a high byte", () => {
eq(decode(B(0xE9), "us-ascii"), "\uFFFD", "us-ascii rejects a high byte");
});

__kitS.section("us-ascii passes ASCII through", () => {
eq(decode(B(0x41), "us-ascii"), "A", "us-ascii passes ASCII through");
});

__kitS.section("an alias decodes the same as the label", () => {
eq(decode(B(0x80), "CP1252"), "\u20AC", "an alias decodes the same as the label");
});

__kitS.section("is honestly reported as absent", () => {
for (const cjk of ["shift_jis", "euc-jp", "gbk", "gb18030", "big5", "euc-kr"]) {
    assert(!encodingExists(cjk), cjk + " is honestly reported as absent");
    throws(() => decode(B(0x41), cjk), "decode refuses " + cjk);
}
});

__kitS.section("decode refuses an unknown label", () => {
throws(() => decode(B(1), "no-such-encoding"), "decode refuses an unknown label");
});

__kitS.section("decode refuses a non-string label", () => {
throws(() => decode(B(1), 42), "decode refuses a non-string label");
});

__kitS.section("decode refuses a Uint16Array rather than reinterpreting it", () => {
throws(() => decode(new Uint16Array([1, 2]), "windows-1252"),
       "decode refuses a Uint16Array rather than reinterpreting it");
});
__kitS.end();

if (fails) {
    print("test_iconv: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_iconv failed");
}
print("test_iconv: " + n + " assertions, 0 failures");
