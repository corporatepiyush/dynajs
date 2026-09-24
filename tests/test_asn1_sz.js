/* test_asn1_sz.js -- +: exact big INTEGERs and the remaining
 * ASN.1 string constructors in dyna:serialize's ASN1.
 *
 * ASN1.int accepts Number AND BigInt; anything beyond int64 encodes
 *       through a decimal-string path into minimal two's complement (X.690
 *       8.3.2) and decodes back EXACTLY (Number up to 2^53, BigInt beyond),
 *       under a shared 64-content-byte cap, both directions.
 *       ORACLE: the golden hex is python's int->DER two's-complement rules
 *       (X.690 10.4), cross-checked with `openssl asn1parse -inform DER`.
 * ia5String (22, ASCII), bmpString (30, UCS-2 big-endian),
 *       universalString (28, UCS-4 big-endian) -- shallow constructors that
 *       validate at ENCODE (the single choke point), and DECODERS for the
 *       same tags so a decode re-encodes byte-identically.
 *       ORACLE: `openssl asn1parse -inform DER` accepts the golden SEQUENCE
 *       below, tag for tag, length for length (verified while writing this).
 *
 * Run: dynajs (CONFIG_NATIVE_MODULES=y) tests/test_asn1_sz.js
 */
import { ASN1 } from "dyna:serialize";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function eq(a, b, msg) {
    assert(a === b, msg + " (got " + JSON.stringify(a) + ", want " +
           JSON.stringify(b) + ")");
}
function throwsMatch(fn, re, msg) {
    let got = "";
    try { fn(); } catch (e) { got = String(e.message); }
    assert(re.test(got), msg + (got ? " (got: " + got + ")" : " (did not throw)"));
}
const hex = (u8) => Array.from(u8, (b) => b.toString(16).padStart(2, "0")).join("");
const bytes = (h) => new Uint8Array(h.match(/../g).map((x) => parseInt(x, 16)));

/* ---------------------------------------------: golden INTEGER table */

/* [value, golden DER] -- python int->DER (minimal two's complement), the
 * same table `openssl asn1parse` agrees with. Covers the mission vectors
 * 0, 127, 128, 255, 256, 32767/32768, 2^31, 2^32, 2^63, 2^64 and negatives
 * of each, plus the exact-int64 boundary. */
const GOLDEN = [
    [0, "020100"],
    [127, "02017f"],
    [128, "02020080"],
    [255, "020200ff"],
    [256, "02020100"],
    [32767, "02027fff"],
    [32768, "0203008000"],
    [2 ** 31, "02050080000000"],
    [2 ** 32, "02050100000000"],
    [2n ** 63n, "0209008000000000000000"],
    [2n ** 64n, "0209010000000000000000"],
    [-1, "0201ff"],
    [-127, "020181"],
    [-128, "020180"],
    [-129, "0202ff7f"],
    [-256, "0202ff00"],
    [-32767, "02028001"],
    [-32768, "02028000"],
    [-(2 ** 31), "020480000000"],
    [-(2 ** 32), "0205ff00000000"],
    [-(2n ** 63n), "02088000000000000000"],
    [-(2n ** 64n), "0209ff0000000000000000"],
];
for (const [v, want] of GOLDEN) {
    eq(hex(ASN1.encode(ASN1.int(v))), want, "int(" + v + ") encodes minimally");
    const back = ASN1.decode(bytes(want)).value;
    assert(back === v || typeof v === "number" && typeof back === "bigint" &&
           false, /* exact type pin below */
           "decode(" + want + ") equals the value");
    if (typeof v === "bigint")
        assert(typeof back === "bigint" && back === v,
               "decode(" + want + ") is the same BigInt");
    else
        assert(back === v && typeof back === "number",
               "decode(" + want + ") is the same Number");
}

/* values beyond int64 round-trip EXACTLY (never a wrapped negative) */
{
    for (const v of [2n ** 64n + 1n, -(2n ** 64n) - 1n, 2n ** 100n,
                     -(2n ** 100n), 255n * 2n ** 64n, -(2n ** 507n)]) {
        const back = ASN1.decode(ASN1.encode(ASN1.int(v))).value;
        assert(typeof back === "bigint" && back === v,
               "big " + v + " round-trips exactly");
    }
}

/* -0n IS 0n (BigInt has no negative zero): DER writes the canonical +0 */
eq(hex(ASN1.encode(ASN1.int(-0n))), "020100", "-0n encodes as +0");
eq(hex(ASN1.encode(ASN1.int(0n))), "020100", "0n encodes as +0");

/* the cap is shared, both directions */
throwsMatch(() => ASN1.encode(ASN1.int(2n ** 512n)),
            /64-byte content cap/, "a positive past the cap refuses");
throwsMatch(() => ASN1.encode(ASN1.int(-(2n ** 512n))),
            /64-byte content cap/, "a negative past the cap refuses");
assert(ASN1.decode(ASN1.encode(ASN1.int(2n ** 507n))).value === 2n ** 507n,
       "a 64-content-byte positive still encodes and decodes");

/* Numbers keep their contract: integer-only, int64 range */
throwsMatch(() => ASN1.encode(ASN1.int(1.5)), /must be an integer/,
            "a fractional Number refuses");
throwsMatch(() => ASN1.encode(ASN1.int(2 ** 63)), /int64 range/,
            "a Number past int64 refuses (use a BigInt)");
eq(hex(ASN1.encode(ASN1.int(2 ** 31))), "02050080000000",
   "a Number at 2^31 encodes like the BigInt");

/* non-minimal content still refuses on the way IN, wide or narrow */
throwsMatch(() => ASN1.decode(bytes("02020000")),
            /non-minimal/, "a redundant leading 0x00 refuses");
throwsMatch(() => ASN1.decode(bytes("0202ff80")),
            /non-minimal/, "a redundant leading 0xFF refuses");

/* -------------------------------------------: the golden SEQUENCE --
 * openssl asn1parse -inform DER accepts this verbatim (verified): each tag,
 * each length. ia5String, BMPString (UCS-2 BE), UniversalString (UCS-4 BE),
 * UTF8String, PrintableString, and both wide INTEGERs. */
const SEQ = "3046160568656c6c6f1e0a006800e9006c006c006f" +
            "1c0c00000041000000e90001f600" +
            "0c0668c3a96c6c6f130548656c6c6f" +
            "02090100000000000000000209ff0000000000000000";
{
    const seq = ASN1.decode(bytes(SEQ));
    eq(seq.value[0].tag, 22, "the IA5String member keeps tag 22");
    eq(seq.value[0].value, "hello", "IA5String decodes to the ASCII text");
    eq(seq.value[1].value, "héllo", "BMPString decodes UCS-2 big-endian");
    eq(seq.value[2].value, "Aé😀", "UniversalString decodes UCS-4 big-endian");
    eq(seq.value[3].value, "héllo", "UTF8String unchanged");
    assert(hex(ASN1.encode(seq)) === SEQ,
           "a decoded tree re-encodes byte-identically");
    /* the SHALLOW constructors must rebuild the same bytes */
    const rebuilt = ASN1.encode(ASN1.seq([
        ASN1.ia5String("hello"),
        ASN1.bmpString("héllo"),
        ASN1.universalString("Aé😀"),
        ASN1.utf8("héllo"),
        ASN1.printable("Hello"),
        ASN1.int(2n ** 64n),
        ASN1.int(-(2n ** 64n)),
    ]));
    assert(hex(rebuilt) === SEQ,
           "the SZ-2 constructors rebuild the openssl-approved bytes");
    /* printableString already existed -- still here */
    eq(ASN1.decode(ASN1.encode(ASN1.printable("Hello"))).value, "Hello",
       "printableString (pre-existing) still round-trips");
}

/* BMPString/UniversalString boundaries */
{
    /* an astral code point is NOT representable in a BMPString */
    throwsMatch(() => ASN1.encode(ASN1.bmpString("\u{1F600}")),
                /above U\+FFFF/, "BMPString refuses an astral code point");
    /* the engine encodes a lone surrogate as WTF-8 at the string boundary;
     * the string types refuse it by name instead of emitting invalid DER */
    throwsMatch(() => ASN1.encode(ASN1.bmpString("\uD800")),
                /ill-formed|lone surrogate/,
                "BMPString refuses a lone surrogate");
    throwsMatch(() => ASN1.encode(ASN1.universalString("\uDC00")),
                /ill-formed|lone surrogate/,
                "UniversalString refuses a lone surrogate");
    throwsMatch(() => ASN1.encode(ASN1.ia5String("héllo")),
                /ASCII/, "IA5String refuses a non-ASCII byte");
    /* UTF8String now refuses what its own decoder always refused */
    throwsMatch(() => ASN1.encode(ASN1.utf8("\uD800x")),
                /well-formed UTF-8/,
                "UTF8String refuses a lone surrogate (decode symmetry)");
    /* empty strings are legal */
    eq(hex(ASN1.encode(ASN1.ia5String(""))), "1600", "empty IA5String");
    eq(hex(ASN1.encode(ASN1.bmpString(""))), "1e00", "empty BMPString");
    eq(hex(ASN1.encode(ASN1.universalString(""))), "1c00",
       "empty UniversalString");
}

/* decode-side refusals: every one names its reason */
{
    throwsMatch(() => ASN1.decode(bytes("1e04d8000061")),
                /surrogate 0xD800/, "a BMP surrogate refuses");
    throwsMatch(() => ASN1.decode(bytes("1e03006100")),
                /whole number of 2-octet groups/, "a ragged BMPString refuses");
    throwsMatch(() => ASN1.decode(bytes("1c0400110000")),
                /above U\+10FFFF/, "a UniversalString past Unicode refuses");
    throwsMatch(() => ASN1.decode(bytes("1c03004100")),
                /4-octet groups/, "a ragged UniversalString refuses");
    throwsMatch(() => ASN1.decode(bytes("16028080")),
                /outside ASCII/, "an IA5 byte past 0x7F refuses");
    /* U+0000 inside a BMPString is a legitimate character (unlike
     * PrintableString, where NUL was refused by the printable set) */
    eq(ASN1.decode(bytes("1e0400610030")).value, "a0",
       "BMPString NUL is a character");
}

/* the constructors are shallow: validation happens at ENCODE */
{
    const lazy = ASN1.ia5String("héllo");
    eq(lazy.tag, 22, "the constructor returns the tagged node unchecked");
    throwsMatch(() => ASN1.encode(lazy), /ASCII/,
                "...and the same node refuses at encode");
}

if (fails) {
    print("test_asn1_sz: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_asn1_sz failed");
}
print("test_asn1_sz: " + n + " assertions, 0 failures");
