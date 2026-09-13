/* test_asn1.js -- ASN.1 DER codec (plan 3.12, row 18).
 *
 * ORACLE 1: GOLDEN DER, from outside the engine. The vectors below are the
 * DER encodings the specification fixes (X.690 8.x) and OpenSSL agrees with.
 * Regenerate/confirm them with `openssl asn1parse -genconf`:
 *
 *   cat > /tmp/asn1_golden.cnf <<'EOF'
 *   asn1=SEQUENCE:seq
 *   [seq]
 *   f1=INTEGER:42
 *   f2=INTEGER:-129
 *   f3=OCTETSTRING:6162
 *   f4=OBJECT:1.2.840.113549.1.1.11
 *   f5=BOOLEAN:TRUE
 *   f6=NULL
 *   f7=UTCTIME:260816123456Z
 *   f8=GENERALIZEDTIME:20260816123456Z
 *   f9=PRINTABLESTRING:Hello
 *   f10=UTF8STRING:héllo
 *   f11=SEQUENCE:inner
 *   [inner]
 *   x=INTEGER:1
 *   EOF
 *   openssl asn1parse -genconf /tmp/asn1_golden.cnf -out /tmp/asn1_golden.der
 *   openssl asn1parse -in /tmp/asn1_golden.der -inform DER -dump
 *   xxd -p /tmp/asn1_golden.der
 * Every type tag and every length in the vectors below must appear in that
 * dump. ORACLE 2: re-encoding a canonical decode must be byte-identical.
 * ORACLE 3: every refusal -- the decoder is the untrusted surface.
 *
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_asn1.js
 */
import { ASN1 } from "dyna:serialize";

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
/* The MESSAGE, not just that it threw: two guards against one symptom must be
 * told apart, and every refusal is supposed to name its reason. */
function throwsMatch(fn, re, msg) {
    let got = "";
    try { fn(); } catch (e) { got = String(e.message); }
    assert(re.test(got), msg + (got ? " (got: " + got + ")" : " (did not throw)"));
}
const hex = (u8) => Array.from(u8, (b) => b.toString(16).padStart(2, "0")).join("");
const bytes = (h) => new Uint8Array(h.match(/../g) ? h.match(/../g).map((x) => parseInt(x, 16)) : []);
const j = (v) => JSON.stringify(v);

/* ------------------------------------------- golden vectors, decode + re-encode */

/* [hex, cls, tag, constructed, check(value)] -- every one is canonical DER,
 * so decode then encode must reproduce the hex EXACTLY. */
const GOLDEN = [
    ["02012a", 0, 2, 0, (v) => v === 42],
    ["020100", 0, 2, 0, (v) => v === 0],
    ["02017f", 0, 2, 0, (v) => v === 127],
    ["02020080", 0, 2, 0, (v) => v === 128],
    ["0201ff", 0, 2, 0, (v) => v === -1],
    ["020180", 0, 2, 0, (v) => v === -128],
    ["0202ff7f", 0, 2, 0, (v) => v === -129],
    ["02020100", 0, 2, 0, (v) => v === 256],
    ["0203008000", 0, 2, 0, (v) => v === 32768],
    ["02028000", 0, 2, 0, (v) => v === -32768],
    ["02087fffffffffffffff", 0, 2, 0, (v) => v === 9223372036854775807n],
    ["02088000000000000000", 0, 2, 0, (v) => v === -9223372036854775808n],
    ["0101ff", 0, 1, 0, (v) => v === true],
    ["010100", 0, 1, 0, (v) => v === false],
    ["0500", 0, 5, 0, (v) => v === null],
    ["04026162", 0, 4, 0, (v) => v instanceof Uint8Array && hex(v) === "6162"],
    ["0400", 0, 4, 0, (v) => v instanceof Uint8Array && v.length === 0],
    ["06092a864886f70d01010b", 0, 6, 0, (v) => v === "1.2.840.113549.1.1.11"],
    ["060a2b06010401d679020402", 0, 6, 0, (v) => v === "1.3.6.1.4.1.11129.2.4.2"],
    ["060127", 0, 6, 0, (v) => v === "0.39"],
    ["060128", 0, 6, 0, (v) => v === "1.0"],
    ["06014f", 0, 6, 0, (v) => v === "1.39"],
    ["060150", 0, 6, 0, (v) => v === "2.0"],
    ["03020080", 0, 3, 0, (v) => v.unused === 0 && hex(v.bytes) === "80"],
    ["03020340", 0, 3, 0, (v) => v.unused === 3 && hex(v.bytes) === "40"],
    ["030100", 0, 3, 0, (v) => v.unused === 0 && v.bytes.length === 0],
    ["170d3236303831363132333435365a", 0, 23, 0, (v) => v === "260816123456Z"],
    ["180f32303236303831363132333435365a", 0, 24, 0, (v) => v === "20260816123456Z"],
    ["130548656c6c6f", 0, 19, 0, (v) => v === "Hello"],
    ["0c0668c3a96c6c6f", 0, 12, 0, (v) => v === "h\u00e9llo"],
    ["300702010104026162", 0, 16, 1,
     (v) => v.length === 2 && v[0].tag === 2 && v[0].value === 1
         && v[1].tag === 4 && hex(v[1].value) === "6162"],
    ["3106020101020102", 0, 17, 1,
     (v) => v.length === 2 && v[0].value === 1 && v[1].value === 2],
    ["3000", 0, 16, 1, (v) => v.length === 0],
    ["8001ab", 2, 0, 0, (v) => hex(v) === "ab"],
    ["a003020101", 2, 0, 1, (v) => hex(v) === "020101"],
    ["300c300702010104026162020101", 0, 16, 1,
     (v) => v.length === 2 && v[0].tag === 16 && v[0].value.length === 2
         && v[1].value === 1],
];
{
    let bad = 0, checked = 0;
    for (const [h, cls, tag, constructed, check] of GOLDEN) {
        const node = ASN1.decode(bytes(h));
        checked += 4;
        if (node.cls !== cls || node.tag !== tag
            || (node.constructed ? 1 : 0) !== constructed || !check(node.value)) {
            bad++;
            if (bad < 5) print("  golden " + h + " decoded wrong: "
                + j({ cls: node.cls, tag: node.tag, c: node.constructed,
                      v: node.value && node.value.length !== undefined
                         ? "bytes[" + node.value.length + "]" : node.value }));
        }
        if (hex(ASN1.encode(node)) !== h) {
            bad++;
            if (bad < 5) print("  golden " + h + " re-encoded to "
                + hex(ASN1.encode(node)));
        }
    }
    assert(bad === 0, "every golden vector decodes to the right node and "
        + "re-encodes BYTE-IDENTICALLY (" + checked + " checks)");
    assert(GOLDEN.length === 36, "the golden list is the one audited");
}

/* ------------------------------------------- helpers -> exact DER bytes */

{
    const vectors = [
        [ASN1.seq([ASN1.int(42), ASN1.octets(bytes("6162"))]), "300702012a04026162"],
        [ASN1.seq([]), "3000"],
        [ASN1.set([ASN1.int(1), ASN1.int(2)]), "3106020101020102"],
        [ASN1.int(0), "020100"],
        [ASN1.int(127), "02017f"],
        [ASN1.int(128), "02020080"],
        [ASN1.int(-1), "0201ff"],
        [ASN1.int(-128), "020180"],
        [ASN1.int(-129), "0202ff7f"],
        [ASN1.int(32768), "0203008000"],
        [ASN1.int(-32768), "02028000"],
        [ASN1.int(2147483647), "02047fffffff"],
        [ASN1.int(-2147483648), "020480000000"],
        [ASN1.int(9223372036854775807n), "02087fffffffffffffff"],
        [ASN1.int(-9223372036854775808n), "02088000000000000000"],
        [ASN1.bool(true), "0101ff"],
        [ASN1.bool(false), "010100"],
        [ASN1.null(), "0500"],
        [ASN1.octets(bytes("6162")), "04026162"],
        [ASN1.octets(new Uint8Array(0)), "0400"],
        [ASN1.bitString(bytes("80"), 0), "03020080"],
        [ASN1.bitString(bytes("40"), 3), "03020340"],
        [ASN1.bitString(bytes("80"), 1), "03020180"],
        [ASN1.bitString(new Uint8Array(0), 0), "030100"],
        [ASN1.oid("1.2.840.113549.1.1.11"), "06092a864886f70d01010b"],
        [ASN1.oid("2.999"), "06028837"],
        [ASN1.utf8("h\u00e9llo"), "0c0668c3a96c6c6f"],
        [ASN1.printable("Hello"), "130548656c6c6f"],
        [ASN1.utcTime("260816123456Z"), "170d3236303831363132333435365a"],
        [ASN1.generalizedTime("20260816123456Z"), "180f32303236303831363132333435365a"],
        [ASN1.context(0, bytes("ab")), "8001ab"],
        [ASN1.contextC(0, [ASN1.int(1)]), "a003020101"],
    ];
    let bad = 0;
    for (const [node, want] of vectors) {
        const got = hex(ASN1.encode(node));
        if (got !== want) {
            bad++;
            if (bad < 5) print("  encode(" + j(node) + ") = " + got
                + ", want " + want);
        }
    }
    assert(bad === 0, "every helper builds the exact DER bytes ("
        + (vectors.length - bad) + "/" + vectors.length + ")");
}
{
    /* The length forms, N-1/N/N+1: 127 is one octet, 128 crosses into the
     * long form, 256 needs the two-octet length. */
    const b127 = new Uint8Array(127).fill(0x5a);
    const b128 = new Uint8Array(128).fill(0x5a);
    const b256 = new Uint8Array(256).fill(0x5a);
    eq(hex(ASN1.encode(ASN1.octets(b127))), "047f" + "5a".repeat(127),
       "127-byte OCTET STRING uses the short length form");
    eq(hex(ASN1.encode(ASN1.octets(b128))), "048180" + "5a".repeat(128),
       "128-byte OCTET STRING crosses into the long form");
    eq(hex(ASN1.encode(ASN1.octets(b256))), "04820100" + "5a".repeat(256),
       "256-byte OCTET STRING uses the two-octet length");
    for (const b of [b127, b128, b256])
        eq(hex(ASN1.encode(ASN1.decode(ASN1.encode(ASN1.octets(b))))),
           hex(ASN1.encode(ASN1.octets(b))),
           "length-form boundary survives decode/re-encode (" + b.length + ")");
}

/* ----------------------------------- round trips over a built corpus */

{
    const corpus = [
        ASN1.int(0), ASN1.int(1), ASN1.int(127), ASN1.int(128), ASN1.int(-1),
        ASN1.int(-128), ASN1.int(-129), ASN1.int(2147483647),
        ASN1.int(-2147483648), ASN1.int(9223372036854775807n),
        ASN1.int(-9223372036854775808n),
        ASN1.bool(true), ASN1.bool(false), ASN1.null(),
        ASN1.octets(new Uint8Array([0, 255, 128, 0])),
        ASN1.octets(new Uint8Array(0)),
        ASN1.bitString(bytes("80"), 0),
        ASN1.bitString(bytes("c0"), 2),
        ASN1.oid("1.2.840.113549.1.1.11"), ASN1.oid("2.999"),
        ASN1.utf8("h\u00e9llo \u6c34"),
        ASN1.printable("CN=Example, OU=Eng"),
        ASN1.utcTime("260816123456Z"),
        ASN1.generalizedTime("20260816123456Z"),
        ASN1.seq([ASN1.int(1), ASN1.octets(bytes("6162")),
                  ASN1.oid("1.3.6.1.4.1.11129.2.4.2")]),
        ASN1.set([ASN1.int(2), ASN1.int(1)]),
        ASN1.context(0, bytes("ab")),
        ASN1.contextC(1, [ASN1.seq([ASN1.utf8("x")])]),
        ASN1.seq([]),
    ];
    let bad = 0;
    for (const node of corpus) {
        const der = hex(ASN1.encode(node));
        const back = ASN1.decode(bytes(der));
        if (hex(ASN1.encode(back)) !== der) {
            bad++;
            if (bad < 5) print("  corpus node did not re-encode: " + der);
        }
    }
    assert(bad === 0, "decode(encode(x)) re-encodes byte-identically for "
        + "every built node (" + corpus.length + " nodes)");
}

/* ------------------ SET OF children are DER-sorted on encode ------------ */
/* X.690 11.6: a SET OF's component encodings appear in ascending order --
 * the sort key is the ENCODED form, shortest first, then lexicographic. */
{
    const outOfOrder = ASN1.encode(
        ASN1.set([ASN1.int(30), ASN1.int(1), ASN1.int(300), ASN1.int(5)]));
    const sorted = ASN1.encode(
        ASN1.set([ASN1.int(1), ASN1.int(5), ASN1.int(30), ASN1.int(300)]));
    eq(hex(outOfOrder), hex(sorted),
       "SET OF encodes identically whatever order the children come in");
    /* the bytes themselves: three 3-byte INTEGERs ascending (0101 < 0105 <
     * 011e), then the one 4-byte INTEGER */
    eq(hex(outOfOrder), "310d02010102010502011e0202012c",
       "and the order IS the DER byte order");
    eq(hex(ASN1.encode(ASN1.decode(bytes(hex(outOfOrder))))), hex(outOfOrder),
       "a sorted SET decodes and re-encodes byte-identically");
    eq(hex(ASN1.encode(ASN1.set([]))), "3100", "an empty SET encodes to 31 00");
}
{
    /* The decoded tree is usable, not just re-encodable. */
    const node = ASN1.decode(bytes("300c300702010104026162020101"));
    eq(node.value.length, 2, "the outer SEQUENCE has two children");
    eq(node.value[0].value.length, 2, "the inner SEQUENCE has two children");
    eq(node.value[0].value[1].value.length, 2, "the OCTET STRING has two bytes");
    eq(node.value[1].value, 1, "and the trailing INTEGER is 1");
}

/* ------------------------------------------------- refusals: decode */

{
    /* THE ATTACK ON A LENGTH-PREFIXED FORMAT: a declared length larger than
     * the remaining input must be refused before anything is allocated. The
     * MESSAGE names it, like the vserialize suite's "declared length". */
    const lies = [
        ["0412aabb", /declared length/],
        ["048180", /declared length|truncated/],
        ["04ffffffff", /declared length|length-of-length/],
        ["3009010101010101", /declared length/],
    ];
    let caught = 0;
    for (const [h, re] of lies) {
        try { ASN1.decode(bytes(h)); } catch (e) {
            if (re.test(String(e.message))) caught++;
            else print("  " + h + " threw the wrong way: " + e.message);
        }
    }
    eq(caught, lies.length, "every lied-about length is refused BEFORE "
       + "anything is allocated");
    throwsMatch(() => ASN1.decode(bytes("30800201010000")), /indefinite/,
        "an indefinite (BER) length is refused");
    throwsMatch(() => ASN1.decode(bytes("0481026162")), /minimal/,
        "a long-form length where a single octet fits is non-minimal");
    throwsMatch(() => ASN1.decode(bytes("048200026162")), /leading zero/,
        "a long-form length with a leading zero octet is non-minimal");
    throwsMatch(() => ASN1.decode(bytes("0485000000016162")), /4 bytes/,
        "a length-of-length beyond 4 bytes is refused");
}
{
    const refused = [
        ["0200", /INTEGER/, "an empty INTEGER is refused"],
        ["0202002a", /minimal/, "the 0x00-padded positive INTEGER is refused"],
        ["0202ff80", /minimal/, "a redundant-sign negative INTEGER is refused"],
        ["0209087fffffffffffff00", /8-byte/, "an INTEGER wider than 8 bytes is refused"],
        ["010102", /BOOLEAN/, "a BOOLEAN that is neither 00 nor FF is refused"],
        ["010200ff", /BOOLEAN/, "a two-octet BOOLEAN is refused"],
        ["0501ff", /NULL/, "a NULL with content is refused"],
        ["0300", /BIT STRING/, "a BIT STRING without the unused-bits octet is refused"],
        ["03020880", /0\.\.7/, "a BIT STRING unused-bits count of 8 is refused"],
        ["030101", /payload/, "unused bits with no payload octet is refused"],
        ["030205ff", /unused bits/, "non-zero trailing unused bits are refused"],
        ["0600", /empty/, "an empty OID is refused"],
        ["060180", /truncated/, "a truncated OID subidentifier is refused"],
        ["06032a8000", /minimal/, "a non-minimal base-128 OID arc is refused"],
        ["06072a88ffffffff7f", /exceeds/, "an OID arc beyond 2^32-1 is refused"],
        ["170c323630383136313233343536", /UTCTime/, "a UTCTime without Z is refused"],
        ["170d3236313331363132333435365a", /UTCTime/, "a UTCTime with month 13 is refused"],
        ["180c323032363038313631323334", /GeneralizedTime/, "a truncated GeneralizedTime is refused"],
        ["1301ff", /PrintableString/, "a PrintableString byte outside the set is refused"],
        ["0c01ff", /UTF-8/, "an invalid UTF-8 UTF8String is refused"],
        ["24020400", /constructed/, "a constructed universal OCTET STRING is BER, not DER"],
        ["1f800001", /minimal/, "a non-minimal high-tag-number is refused"],
        ["1f88ffffff7f", /tag/, "a tag number beyond 2^31-1 is refused"],
        ["02012a00", /trailing/, "trailing bytes after the value are refused"],
    ];
    let caught = 0;
    for (const [h, re, msg] of refused) {
        try {
            ASN1.decode(bytes(h));
        } catch (e) {
            if (re.test(String(e.message))) caught++;
            else print("  " + h + " threw the wrong way: " + e.message);
        }
    }
    eq(caught, refused.length, "every refusal names its reason ("
        + (refused.length - caught) + " of " + refused.length + " misnamed)");
}
{
    throws(() => ASN1.decode(new Uint8Array(0)), "empty input is refused");
    throws(() => ASN1.decode("02012a"), "a string is not bytes");
    throws(() => ASN1.decode(new Uint16Array(2)), "a wider view is refused");
    throws(() => ASN1.decode(), "missing argument is refused");
}
{
    /* Nesting is bounded, so a deep document is a RangeError, not a crash. */
    const wrap = (h) => {
        const len = h.length / 2;
        const lenHex = len < 128 ? len.toString(16).padStart(2, "0")
            : (() => { let b = [], v = len;
                while (v) { b.unshift((v & 255).toString(16).padStart(2, "0"));
                    v = Math.floor(v / 256); }
                return (128 + b.length).toString(16) + b.join(""); })();
        return "30" + lenHex + h;
    };
    let deep = "020101";
    for (let i = 0; i < 300; i++) deep = wrap(deep);
    throwsMatch(() => ASN1.decode(bytes(deep)), /nesting/,
        "300-deep nesting is refused");
    let ok = "020101";
    for (let i = 0; i < 50; i++) ok = wrap(ok);
    let node = ASN1.decode(bytes(ok));
    for (let i = 0; i < 50; i++) node = node.value[0];
    eq(node.value, 1, "and 50-deep nesting still decodes");
}

/* ------------------------------------------------- refusals: encode */

{
    throwsMatch(() => ASN1.encode(42), /node/, "a number is not a node");
    throwsMatch(() => ASN1.encode({}), /tag/, "a node without a tag is refused");
    throwsMatch(() => ASN1.encode({ tag: 2, value: "x" }), /INTEGER/,
        "a string INTEGER value is refused");
    throwsMatch(() => ASN1.encode(ASN1.int(1.5)), /integer/,
        "a fractional INTEGER is refused");
    throwsMatch(() => ASN1.encode(ASN1.int(2 ** 63)), /int64/,
        "a Number beyond int64 is refused");
    throwsMatch(() => ASN1.encode(ASN1.int(NaN)), /integer/,
        "a NaN INTEGER is refused");
    throwsMatch(() => ASN1.encode(ASN1.octets("x")), /Uint8Array/,
        "a string OCTET STRING is refused");
    throwsMatch(() => ASN1.encode({ cls: 0, tag: 4, constructed: true,
        value: [ASN1.int(1)] }), /constructed/,
        "a constructed universal OCTET STRING is refused on encode too");
    throwsMatch(() => ASN1.encode(ASN1.bitString(bytes("40"), 8)), /0\.\.7/,
        "an unused-bits count of 8 is refused on encode");
    throwsMatch(() => ASN1.encode(ASN1.bitString(bytes("ff"), 5)), /unused bits/,
        "non-zero trailing unused bits are refused on encode");
    throwsMatch(() => ASN1.encode(ASN1.printable("h\u00e9llo")), /PrintableString/,
        "a non-printable PrintableString is refused");
    throwsMatch(() => ASN1.encode(ASN1.utcTime("260816123456")), /UTCTime/,
        "a malformed UTCTime is refused");
    throwsMatch(() => ASN1.encode(ASN1.generalizedTime("20260816123456")),
        /GeneralizedTime/, "a malformed GeneralizedTime is refused");
    throwsMatch(() => ASN1.encode(ASN1.oid("3.2.1")), /0, 1 or 2/,
        "an OID with first arc 3 is refused");
    throwsMatch(() => ASN1.encode(ASN1.oid("1.40")), /second arc/,
        "1.40 is not representable in DER");
    throwsMatch(() => ASN1.encode(ASN1.oid("1.2.4294967296")), /2\^32/,
        "an OID arc beyond 2^32-1 is refused");
    throwsMatch(() => ASN1.encode(ASN1.oid("1.2.3.")), /malformed/,
        "a trailing dot in an OID is refused");
    throwsMatch(() => ASN1.encode(ASN1.oid("")), /at least two arcs/,
        "an OID with fewer than two arcs is refused");
    throwsMatch(() => ASN1.encode(ASN1.seq([42])), /node/,
        "a non-node child is refused");
    {
        let node = ASN1.int(1);
        for (let i = 0; i < 300; i++) node = ASN1.seq([node]);
        throwsMatch(() => ASN1.encode(node), /nesting/,
            "300-deep encode nesting is refused");
    }
    let ok = ASN1.int(1);
    for (let i = 0; i < 50; i++) ok = ASN1.seq([ok]);
    assert(ASN1.encode(ok).length > 0, "50-deep encode still works");
}

/* ------------------------------------- re-encode stability under repetition */

{
    /* A decoded tree must re-encode identically NO MATTER how many times it
     * is walked -- this is where a leaked refcount or a mutated node shows
     * under ASan. */
    const der = bytes("300c300702010104026162020101");
    const first = hex(ASN1.encode(ASN1.decode(der)));
    let same = true;
    for (let i = 0; i < 2000; i++)
        if (hex(ASN1.encode(ASN1.decode(der))) !== first) same = false;
    assert(same, "decode/re-encode is stable over 2000 repetitions");

    /* 08-17 security regression: the children cap is a memory bound --
       3 bytes of input per child object meant a 2^26 cap never bit, so
       the cap is 2^16 and a bomb past it refuses by name. */
    {
        const cap = 1 << 16;
        const per = 3;               /* 02 01 00: INTEGER 0 */
        const bomb = new Uint8Array((cap + 1) * per);
        for (let i = 0; i < bomb.length; i += per) { bomb[i] = 0x02; bomb[i + 1] = 0x01; }
        bomb[0] = 0x30; bomb[1] = 0x82;      /* SEQUENCE, long form length */
        let t = false;
        try { ASN1.decode(bomb); } catch (e) { t = true; }
        assert(t, "children cap: a constructed bomb past 2^16 refuses");
    }
}

if (fails) {
    print("test_asn1: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_asn1 failed");
}
print("test_asn1: " + n + " assertions, 0 failures");