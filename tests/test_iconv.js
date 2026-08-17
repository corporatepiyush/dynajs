/* test_iconv.js -- legacy single-byte charsets in dyna:bytes (design 11).
 *
 * A codec verified only against itself agrees with its own bugs, so the vectors
 * here are the published Unicode mappings for specific bytes, not a round trip.
 * The round trip is checked SEPARATELY and only as a second, weaker property.
 *
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_iconv.js
 */
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

/* ------------------------------------------------------ published vectors */

/* windows-1252: the 0x80-0x9F block is what distinguishes it from latin-1, and
 * getting that block wrong is the single most common charset bug there is. */
eq(decode(B(0x80), "windows-1252"), "\u20AC", "cp1252 0x80 is EURO SIGN");
eq(decode(B(0x82), "windows-1252"), "\u201A", "cp1252 0x82 is SINGLE LOW-9 QUOTE");
eq(decode(B(0x85), "windows-1252"), "\u2026", "cp1252 0x85 is HORIZONTAL ELLIPSIS");
eq(decode(B(0x91), "windows-1252"), "\u2018", "cp1252 0x91 is LEFT SINGLE QUOTE");
eq(decode(B(0x92), "windows-1252"), "\u2019", "cp1252 0x92 is RIGHT SINGLE QUOTE");
eq(decode(B(0x93), "windows-1252"), "\u201C", "cp1252 0x93 is LEFT DOUBLE QUOTE");
eq(decode(B(0x99), "windows-1252"), "\u2122", "cp1252 0x99 is TRADE MARK SIGN");
eq(decode(B(0xE9), "windows-1252"), "\u00E9", "cp1252 0xE9 is e-acute");

/* iso-8859-1 is the identity over the high half -- 0x80..0x9F are C1 controls,
 * NOT the cp1252 punctuation. This is exactly where the two must disagree. */
eq(decode(B(0x80), "iso-8859-1"), "\u0080", "latin-1 0x80 is U+0080, not EURO");
eq(decode(B(0xE9), "iso-8859-1"), "\u00E9", "latin-1 0xE9 is e-acute");
assert(decode(B(0x80), "iso-8859-1") !== decode(B(0x80), "windows-1252"),
       "latin-1 and cp1252 really do differ at 0x80");

/* iso-8859-15 differs from -1 at exactly eight positions; 0xA4 is the famous one. */
eq(decode(B(0xA4), "iso-8859-15"), "\u20AC", "iso-8859-15 0xA4 is EURO SIGN");
eq(decode(B(0xA4), "iso-8859-1"), "\u00A4", "iso-8859-1 0xA4 is CURRENCY SIGN");

/* Cyrillic families */
eq(decode(B(0xC0), "windows-1251"), "\u0410", "cp1251 0xC0 is CYRILLIC A");
/* KOI8-R puts LOWERCASE at 0xC0-0xDF and UPPERCASE at 0xE0-0xFF, which is
   the reverse of what the other Cyrillic charsets do. Both are pinned. */
eq(decode(B(0xC0), "koi8-r"), "\u044E", "koi8-r 0xC0 is lowercase yu");
eq(decode(B(0xE0), "koi8-r"), "\u042E", "koi8-r 0xE0 is UPPERCASE Yu");
eq(decode(B(0x80), "ibm866"), "\u0410", "ibm866 0x80 is CYRILLIC A");

/* Greek and Hebrew */
eq(decode(B(0xE1), "iso-8859-7"), "\u03B1", "iso-8859-7 0xE1 is GREEK ALPHA");
eq(decode(B(0xE0), "iso-8859-8"), "\u05D0", "iso-8859-8 0xE0 is HEBREW ALEF");

/* macintosh */
eq(decode(B(0xA5), "macintosh"), "\u2022", "macintosh 0xA5 is BULLET");

/* ASCII is ASCII everywhere. */
for (const label of ["windows-1252", "iso-8859-1", "koi8-r", "iso-8859-7", "macintosh"]) {
    const bytes = [];
    for (let b = 0; b < 0x80; b++) bytes.push(b);
    let want = "";
    for (let b = 0; b < 0x80; b++) want += String.fromCharCode(b);
    eq(decode(new Uint8Array(bytes), label), want,
       "the low half is identity in " + label);
}

/* An undefined byte becomes U+FFFD rather than a wrong character or a throw. */
eq(decode(B(0xA1), "iso-8859-3"), "\u0126", "iso-8859-3 0xA1 is H-stroke");
eq(decode(B(0xA5), "iso-8859-3"), "\uFFFD", "an undefined byte decodes to U+FFFD");
/* us-ascii treats every high byte as undefined. */
eq(decode(B(0xE9), "us-ascii"), "\uFFFD", "us-ascii rejects a high byte");
eq(decode(B(0x41), "us-ascii"), "A", "us-ascii passes ASCII through");

/* ------------------------------------------------------------------ encode */

eq(Array.from(encode("\u20AC", "windows-1252")).join(","), "128",
   "EURO encodes to 0x80 in cp1252");
eq(Array.from(encode("\u00E9", "iso-8859-1")).join(","), "233",
   "e-acute encodes to 0xE9 in latin-1");
eq(Array.from(encode("abc", "windows-1252")).join(","), "97,98,99", "ASCII encodes as itself");
eq(encode("", "windows-1252").length, 0, "empty string encodes to empty");

/* A code point the charset cannot express becomes '?', not a throw and not a
 * silently wrong byte. */
eq(Array.from(encode("\u4F60", "iso-8859-1")).join(","), "63",
   "an inexpressible code point becomes '?'");
eq(Array.from(encode("a\u4F60b", "windows-1252")).join(","), "97,63,98",
   "substitution happens in place");
/* An astral character is ONE substitution, not two -- it must not be counted
 * as its two UTF-16 units. */
eq(encode("\u{1f600}", "windows-1252").length, 1,
   "an astral character substitutes as a single byte");

/* ------------------------------------------------- round trip (second-order) */

/* For every charset, every byte it DEFINES must survive decode->encode. This
 * is weaker than the vectors above but it covers all 128 high slots at once. */
{
    let checked = 0, bad = 0;
    for (const label of encodings()) {
        if (label === "utf-8" || label === "us-ascii") continue;
        for (let b = 0x80; b < 0x100; b++) {
            const s = decode(B(b), label);
            if (s === "\uFFFD") continue;            /* undefined: nothing to check */
            const back = encode(s, label);
            checked++;
            if (back.length !== 1 || back[0] !== b) {
                bad++;
                if (bad === 1)
                    print("  first mismatch: " + label + " byte 0x" + b.toString(16));
            }
        }
    }
    eq(bad, 0, "every defined byte round-trips in every charset");
    assert(checked > 3000, "the round trip covered the tables (" + checked + " bytes)");
}

/* --------------------------------------------------------------- labels */

assert(encodingExists("utf-8"), "utf-8 exists");
assert(encodingExists("windows-1252"), "windows-1252 exists");
assert(encodingExists("WINDOWS-1252"), "labels are case-insensitive");
assert(encodingExists("  windows-1252  "), "labels tolerate surrounding space");
assert(encodingExists("latin1"), "alias latin1");
assert(encodingExists("cp1252"), "alias cp1252");
assert(encodingExists("ascii"), "alias ascii");
eq(decode(B(0x80), "CP1252"), "\u20AC", "an alias decodes the same as the label");

/* The CJK families are NOT built, and encodingExists must say so rather than
 * claiming support and then throwing. */
for (const cjk of ["shift_jis", "euc-jp", "gbk", "gb18030", "big5", "euc-kr"]) {
    assert(!encodingExists(cjk), cjk + " is honestly reported as absent");
    throws(() => decode(B(0x41), cjk), "decode refuses " + cjk);
}
assert(!encodingExists("not-a-charset"), "an invented label does not exist");

/* encodings() enumerates what this build can actually do. */
{
    const list = encodings();
    assert(Array.isArray(list), "encodings() returns an array");
    assert(list.indexOf("utf-8") >= 0, "encodings() includes utf-8");
    assert(list.indexOf("windows-1252") >= 0, "encodings() includes windows-1252");
    assert(list.indexOf("shift_jis") < 0, "encodings() does not claim shift_jis");
    for (const label of list)
        assert(encodingExists(label),
               "every label encodings() lists actually exists: " + label);
}

/* ------------------------------------------------------------- refusals */

throws(() => decode(B(1), "no-such-encoding"), "decode refuses an unknown label");
throws(() => encode("a", "no-such-encoding"), "encode refuses an unknown label");
throws(() => decode(B(1), 42), "decode refuses a non-string label");
throws(() => encode(42, "windows-1252"), "encode refuses a non-string input");
/* A wider view is rejected rather than reinterpreted as bytes. */
throws(() => decode(new Uint16Array([1, 2]), "windows-1252"),
       "decode refuses a Uint16Array rather than reinterpreting it");

if (fails) {
    print("test_iconv: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_iconv failed");
}
print("test_iconv: " + n + " assertions, 0 failures");
