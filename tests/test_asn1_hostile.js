/* test_asn1_hostile.js -- the ASN.1 DER decoder/encoder against hostile input.
 *
 * The shapes here are the ones an attacker controls on any DER boundary:
 * truncated headers, lying lengths (inner past outer, outer past buffer),
 * indefinite/overlong length encodings, nested-depth bombs, oversized tags,
 * and every string type carrying bytes outside its alphabet. Each refusal
 * must NAME its reason (a silent accept of a lying length is how this class
 * of bug turns into a memory bug), and every accepted value must re-encode
 * byte-identically.
 *
 * The encoder half pins the error-message QUOTES: the messages name the
 * offending byte and its offset, which must be read from the input before
 * any release -- reading it after the free is the freed-region access this
 * suite exists for. Run the whole battery under CONFIG_ASAN=y: with the
 * quoting bug present, test_asn1_hostile aborts the process mid-suite.
 *
 * Run: dynajs (CONFIG_NATIVE_MODULES=y) tests/test_asn1_hostile.js
 *      (under CONFIG_ASAN=y for the memory-safety half) */
import { ASN1 } from "dyna:serialize";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function eq(a, b, msg) {
    assert(a === b, msg + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
}
function throwsMatch(fn, re, msg) {
    let got = "";
    try { fn(); } catch (e) { got = String(e.message); }
    assert(re.test(got), msg + (got ? " (got: " + got + ")" : " (did not throw)"));
}
const hex = (u8) => Array.from(u8).map(b => b.toString(16).padStart(2, "0")).join("");
const unhex = (s) => {
    const out = new Uint8Array(s.length / 2);
    for (let i = 0; i < out.length; i++) out[i] = parseInt(s.substr(i * 2, 2), 16);
    return out;
};

/* ------------------------------------------------ decode: hostile framing */

{
    /* truncated headers */
    throwsMatch(() => ASN1.decode(unhex("")), /empty|trunc|length/,
        "an empty buffer is refused");
    throwsMatch(() => ASN1.decode(unhex("30")), /trunc|length|end/,
        "a bare SEQUENCE tag is refused");
    throwsMatch(() => ASN1.decode(unhex("3005020101")), /trunc|end|length|past/,
        "content shorter than its own length");
    throwsMatch(() => ASN1.decode(unhex("3081")), /trunc|length|end/,
        "long-form length with no octets");
    throwsMatch(() => ASN1.decode(unhex("308200")), /trunc|length|end/,
        "two-octet length cut in half");
    throwsMatch(() => ASN1.decode(unhex("0480")), /indefinite|length/,
        "indefinite length is refused (not DER)");
    throwsMatch(() => ASN1.decode(unhex("30800201010000")), /indefinite|length/,
        "indefinite length on a constructed is refused");
}

{
    /* lying lengths: inner past outer, outer past buffer */
    throwsMatch(() => ASN1.decode(unhex("300302050102030405")), /past|trunc|length|end/,
        "inner INTEGER overruns the SEQUENCE content");
    throwsMatch(() => ASN1.decode(unhex("3008020101020102020103")), /past|trunc|length|end|extra|cover|trailing/,
        "outer length short of the buffer");
    throwsMatch(() => ASN1.decode(unhex("307f020101")), /past|trunc|length|end/,
        "outer length claims 127 bytes of nothing");
    throwsMatch(() => ASN1.decode(unhex("30820100020101")), /past|trunc|length|end/,
        "long-form outer length past the buffer");
    throwsMatch(() => ASN1.decode(unhex("0200")), /empty|length|INTEGER/,
        "a zero-length INTEGER");
    throwsMatch(() => ASN1.decode(unhex("04036162")), /past|trunc|length|end/,
        "an OCTET STRING claiming 3 bytes over 2");
    throwsMatch(() => ASN1.decode(unhex("3000020101")), /past|trunc|length|extra|end|trailing/,
        "an empty SEQUENCE with bytes after it");
}

{
    /* non-minimal and oversized length forms */
    throwsMatch(() => ASN1.decode(unhex("308106020101020102")), /length|form|minimal/,
        "long form used for a length below 128");
    throwsMatch(() => ASN1.decode(unhex("30820006020101020102")), /length|form|minimal/,
        "two-octet long form padded with a zero");
    throwsMatch(() => ASN1.decode(unhex("30857fffffff00020101")), /length|large|form/,
        "a five-octet length encoding");
}

{
    /* nesting bomb and oversized tags: a proper DER builder so every
       level's length is honest and the DEPTH cap is what fires */
    const wrap = (body) => {
        let len = body.length, lbytes = [];
        if (len < 128) lbytes = [len];
        else if (len < 256) lbytes = [0x81, len];
        else lbytes = [0x82, (len >> 8) & 0xff, len & 0xff];
        return [0x30].concat(lbytes, body);
    };
    let deep = [0x02, 0x01, 0x01];
    for (let k = 0; k < 300; k++) deep = wrap(deep);
    throwsMatch(() => ASN1.decode(new Uint8Array(deep)), /nesting|depth/,
        "300-deep SEQUENCE nesting is refused by name (cap 256)");
    throwsMatch(() => ASN1.decode(unhex("1f80050100")), /tag|minimal|form/,
        "a non-minimal high-tag-number form (leading zero septet)");
    throwsMatch(() => ASN1.decode(unhex("1f8f8f8f8f8f8f7f0100")), /tag|large|form|exceeds/,
        "a tag number past the cap");
}

{
    /* string content against its alphabet, on the DECODE side */
    throwsMatch(() => ASN1.decode(unhex("1602c3a9")), /ASCII|IA5/,
        "an IA5String carrying a non-ASCII byte is refused");
    throwsMatch(() => ASN1.decode(unhex("130261ff")), /printable|PRINTABLE/,
        "a PrintableString outside the printable set");
    throwsMatch(() => ASN1.decode(unhex("0c0361ffc2")), /UTF-8|utf8|UTF8/,
        "a UTF8String with invalid UTF-8");
    throwsMatch(() => ASN1.decode(unhex("170d3236303831363132333435365a00")),
        /trailing|UTCTime|time|canonical/, "a UTCTime with a trailing byte");
}

{
    /* accepted hostile-ish shapes round-trip byte-identically */
    const good = [
        "3006020101020102",         /* seq of two ints */
        "3106020101020102",         /* SET: DER sort applies on re-encode */
        "1600",                     /* empty IA5String */
        "0400",                     /* empty OCTET STRING */
        "020180",                   /* INTEGER -128 minimal */
        "02020080",                 /* INTEGER 128 with sign pad */
    ];
    for (const g of good) {
        const tree = ASN1.decode(unhex(g));
        eq(hex(ASN1.encode(tree)), g, "re-encode is byte-identical for " + g);
    }
    /* SET OF ordering: three members shuffled in must come out sorted */
    const set = ASN1.set([ASN1.int(3), ASN1.int(1), ASN1.int(2)]);
    eq(hex(ASN1.encode(set)), "3109020101020102020103",
       "SET OF children encode in DER order");
}

/* ----------------------------------------------- encode: the quoted errors */

{
    /* The IA5String refusal quotes the offending byte and offset. The quote
       is read from the string's UTF-8 bytes: it must be captured before any
       release (the freed-region access class). Long strings matter: the bad
       byte sits deep inside a heap allocation, not the inline case. */
    const run = (s, why) => {
        let got = "";
        try { ASN1.encode(ASN1.ia5String(s)); } catch (e) { got = String(e.message); }
        const m = /byte 0x([0-9A-Fa-f]{2}) at offset (\d+)/.exec(got);
        assert(m !== null, why + ": the message quotes byte and offset (got: " + got + ")");
        if (m) {
            const bytes = new TextEncoder().encode(s);
            let bad = -1;
            for (let i = 0; i < bytes.length; i++)
                if (bytes[i] > 0x7f) { bad = i; break; }
            assert(bad >= 0, why + ": the input does contain a non-ASCII byte");
            eq(parseInt(m[2], 10), bad,
               why + ": the quoted offset is the UTF-8 byte offset of the offender");
            eq(parseInt(m[1], 16), bytes[bad],
               why + ": the quoted byte is the actual UTF-8 byte at that offset");
        }
        return got;
    };
    /* the historic repro shape: a bad byte inside a 26-char heap string (the
       error path used to quote the byte AFTER releasing the buffer) */
    run("aaaaaaaaaaaaaaaaaaaa\u00e9aaaaa", "heap string, bad byte at offset 21");
    /* boundary matrix over the bad byte's position: min-1/min/max/max+1 */
    for (const off of [0, 1, 2, 30, 63, 64, 126, 127, 128, 254, 255, 256, 4095, 4096]) {
        run("x".repeat(off) + "\u7fff" + "y".repeat(off), "bad byte at offset " + off);
    }
    /* every other string type's refusal is named and quotes nothing stale */
    throwsMatch(() => ASN1.encode(ASN1.printable("a\tb")), /PRINTABLE|printable/,
        "PrintableString refusal names the alphabet");
    throwsMatch(() => ASN1.encode(ASN1.utf8("a\uD800b")), /UTF-8|surrogate|well-formed/,
        "UTF8String refuses a lone surrogate");
    throwsMatch(() => ASN1.encode(ASN1.bmpString("a\u{1F600}")), /BMP|U\+/,
        "BMPString refuses above U+FFFF");
    throwsMatch(() => ASN1.encode(ASN1.universalString("a\uD800b")),
        /well-formed|surrogate/, "UniversalString refuses a lone surrogate");
    throwsMatch(() => ASN1.encode(ASN1.utcTime("nope")), /UTCTime|canonical/,
        "UTCTime format refusal");
    throwsMatch(() => ASN1.encode(ASN1.generalizedTime("2026")), /GeneralizedTime|canonical/,
        "GeneralizedTime format refusal");
}

{
    /* lying encode inputs: values whose getters fight the encoder */
    throwsMatch(() => ASN1.encode({ cls: 0, tag: 2, constructed: false, value: 1.5 }),
        /INTEGER|integer/, "a fractional INTEGER is refused");
    throwsMatch(() => ASN1.encode({ cls: 0, tag: 2, constructed: false, value: 2n ** 8192n }),
        /BigInt|large|INTEGER|range|bytes/, "an 8192-bit BigInt past the content cap");
    throwsMatch(() => ASN1.encode({ cls: 0, tag: 3, constructed: false,
        value: { unused: 2, bytes: new Uint8Array([0xF3]) } }),
        /unused|zero|BIT STRING/, "BIT STRING unused bits must be zero in DER");
    throwsMatch(() => ASN1.encode({ cls: 0, tag: 3, constructed: false,
        value: { unused: 3, bytes: new Uint8Array(0) } }),
        /unused|payload|BIT STRING/, "BIT STRING unused bits with no payload octet");
    throwsMatch(() => ASN1.encode(ASN1.seq([1])), /node|object/,
        "an array member where a node belongs");
    let ok = "";
    try { ok = hex(ASN1.encode({ cls: 0, tag: 16, constructed: true,
        value: [{ cls: 0, tag: 16, constructed: true, value: [] }] })); } catch (e) {}
    eq(ok, "30023000", "nested SEQUENCEs still encode");
}

print("test_asn1_hostile: " + (n - fails) + "/" + n + " assertions" +
      (fails ? " -- " + fails + " FAILURES" : " all passed"));
if (fails) throw new Error(fails + " failures");
