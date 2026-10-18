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
           false,
           "decode(" + want + ") equals the value");
    if (typeof v === "bigint")
        assert(typeof back === "bigint" && back === v,
               "decode(" + want + ") is the same BigInt");
    else
        assert(back === v && typeof back === "number",
               "decode(" + want + ") is the same Number");
}

{
    for (const v of [2n ** 64n + 1n, -(2n ** 64n) - 1n, 2n ** 100n,
                     -(2n ** 100n), 255n * 2n ** 64n, -(2n ** 507n)]) {
        const back = ASN1.decode(ASN1.encode(ASN1.int(v))).value;
        assert(typeof back === "bigint" && back === v,
               "big " + v + " round-trips exactly");
    }
}

eq(hex(ASN1.encode(ASN1.int(-0n))), "020100", "-0n encodes as +0");
eq(hex(ASN1.encode(ASN1.int(0n))), "020100", "0n encodes as +0");

throwsMatch(() => ASN1.encode(ASN1.int(2n ** 512n)),
            /64-byte content cap/, "a positive past the cap refuses");
throwsMatch(() => ASN1.encode(ASN1.int(-(2n ** 512n))),
            /64-byte content cap/, "a negative past the cap refuses");
assert(ASN1.decode(ASN1.encode(ASN1.int(2n ** 507n))).value === 2n ** 507n,
       "a 64-content-byte positive still encodes and decodes");

throwsMatch(() => ASN1.encode(ASN1.int(1.5)), /must be an integer/,
            "a fractional Number refuses");
throwsMatch(() => ASN1.encode(ASN1.int(2 ** 63)), /int64 range/,
            "a Number past int64 refuses (use a BigInt)");
eq(hex(ASN1.encode(ASN1.int(2 ** 31))), "02050080000000",
   "a Number at 2^31 encodes like the BigInt");

throwsMatch(() => ASN1.decode(bytes("02020000")),
            /non-minimal/, "a redundant leading 0x00 refuses");
throwsMatch(() => ASN1.decode(bytes("0202ff80")),
            /non-minimal/, "a redundant leading 0xFF refuses");

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
    eq(ASN1.decode(ASN1.encode(ASN1.printable("Hello"))).value, "Hello",
       "printableString (pre-existing) still round-trips");
}

{
    throwsMatch(() => ASN1.encode(ASN1.bmpString("\u{1F600}")),
                /above U\+FFFF/, "BMPString refuses an astral code point");
    throwsMatch(() => ASN1.encode(ASN1.bmpString("\uD800")),
                /ill-formed|lone surrogate/,
                "BMPString refuses a lone surrogate");
    throwsMatch(() => ASN1.encode(ASN1.universalString("\uDC00")),
                /ill-formed|lone surrogate/,
                "UniversalString refuses a lone surrogate");
    throwsMatch(() => ASN1.encode(ASN1.ia5String("héllo")),
                /ASCII/, "IA5String refuses a non-ASCII byte");
    throwsMatch(() => ASN1.encode(ASN1.utf8("\uD800x")),
                /well-formed UTF-8/,
                "UTF8String refuses a lone surrogate (decode symmetry)");
    eq(hex(ASN1.encode(ASN1.ia5String(""))), "1600", "empty IA5String");
    eq(hex(ASN1.encode(ASN1.bmpString(""))), "1e00", "empty BMPString");
    eq(hex(ASN1.encode(ASN1.universalString(""))), "1c00",
       "empty UniversalString");
}

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
    eq(ASN1.decode(bytes("1e0400610030")).value, "a0",
       "BMPString NUL is a character");
}

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
