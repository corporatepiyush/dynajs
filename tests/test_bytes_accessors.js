/* test_bytes_accessors.js -- the typed read/write family on Bytes and Text.
 *
 * WHY THIS FILE EXISTS: an enumeration of the whole public surface from the
 * BINARY (1691 names) against every test, example and fuzz target found 39
 * names nobody had ever referenced -- and 32 of them were these. They index
 * into a buffer at a CALLER-SUPPLIED OFFSET, which is where an out-of-bounds
 * read lives, so they were the untested surface that mattered most.
 *
 * The bounds checks turned out to be correct. This locks that in.
 *
 * THE DISCRIMINATOR IS THE STRADDLE. An offset where the read STARTS in bounds
 * and ENDS past the end is the case a naive `offset < length` check passes.
 * Reading past-the-end only proves the coarse check exists.
 *
 * Endianness is checked against hand-written bytes, not against our own
 * writer: a read/write pair that agrees with itself passes just as well when
 * both are byte-swapped.
 */
import { Bytes, Text } from "dyna:bytes";

let pass = 0, fail = 0;
const ok = (c, w, d) => { if (c) { pass++; print("  ok    " + w); }
                          else { fail++; print("  FAIL  " + w + (d !== undefined ? "  [" + d + "]" : "")); } };
function throws(fn, w) {
    let t = false, m = "";
    try { fn(); } catch (e) { t = true; m = String(e.message || e); }
    ok(t, w, t ? undefined : "did NOT throw");
    return m;
}

/* width in bytes for each accessor, and whether it is big-endian */
const ACC = [
    ["Int8", 1], ["Uint8", 1],
    ["Int16BE", 2], ["Uint16BE", 2], ["Uint16LE", 2],
    ["Int32BE", 4], ["Int32LE", 4],
    ["FloatBE", 4], ["FloatLE", 4],
    ["BigInt64BE", 8], ["BigInt64LE", 8], ["BigUint64BE", 8],
];

print("--- bounds: every accessor, at the STRADDLE and past the end ---");
{
    const LEN = 16;
    for (const [name, width] of ACC) {
        const b = new Bytes(new Uint8Array(LEN));
        const rd = "read" + name, wr = "write" + name;
        if (typeof b[rd] !== "function") { ok(false, rd + " exists"); continue; }

        /* last legal offset reads exactly to the end */
        let okLast = true;
        try { b[rd](LEN - width); } catch (e) { okLast = false; }
        ok(okLast, rd + ": the last in-range offset (" + (LEN - width) + ") is accepted");

        if (width > 1) {
            /* STARTS in bounds, ENDS past it -- the off-by-one case */
            throws(() => b[rd](LEN - width + 1),
                   rd + ": offset " + (LEN - width + 1) + " STRADDLES the end and is refused");
            if (typeof b[wr] === "function")
                throws(() => b[wr](LEN - width + 1, width === 8 ? 0n : 0),
                       wr + ": the same straddle is refused on WRITE");
        }
        throws(() => b[rd](LEN), rd + ": offset == length is refused");
        throws(() => b[rd](LEN + 1000), rd + ": far past the end is refused");
        throws(() => b[rd](-1), rd + ": a negative offset is refused");
        throws(() => b[rd](2 ** 31), rd + ": an offset past INT32_MAX is refused");
    }
}

print("--- endianness, against hand-written bytes (not our own writer) ---");
{
    /* 0x01020304 big-endian */
    const b = new Bytes(new Uint8Array([1, 2, 3, 4, 5, 6, 7, 8]));
    ok(b.readInt32BE(0) === 0x01020304, "readInt32BE reads most-significant first",
       b.readInt32BE(0));
    ok(b.readInt32LE(0) === 0x04030201, "readInt32LE reads least-significant first",
       b.readInt32LE(0));
    ok(b.readUint16BE(0) === 0x0102, "readUint16BE", b.readUint16BE(0));
    ok(b.readUint16LE(0) === 0x0201, "readUint16LE", b.readUint16LE(0));
    ok(b.readBigInt64BE(0) === 0x0102030405060708n, "readBigInt64BE",
       String(b.readBigInt64BE(0)));
    ok(b.readBigInt64LE(0) === 0x0807060504030201n, "readBigInt64LE",
       String(b.readBigInt64LE(0)));
    /* BE and LE must DISAGREE -- if they matched, both could be the same bug */
    ok(b.readInt32BE(0) !== b.readInt32LE(0),
       "BE and LE disagree on the same bytes (so neither is the other)");
}

print("--- sign: the signed readers must go negative, the unsigned must not ---");
{
    const b = new Bytes(new Uint8Array([0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff]));
    ok(b.readInt8(0) === -1, "readInt8(0xff) is -1", b.readInt8(0));
    ok(b.readUint8(0) === 255, "readUint8(0xff) is 255", b.readUint8(0));
    ok(b.readInt32BE(0) === -1, "readInt32BE(all ones) is -1", b.readInt32BE(0));
    ok(b.readBigInt64BE(0) === -1n, "readBigInt64BE(all ones) is -1",
       String(b.readBigInt64BE(0)));
    ok(b.readBigUint64BE(0) === 0xffffffffffffffffn, "readBigUint64BE is unsigned",
       String(b.readBigUint64BE(0)));
}

print("--- round trip: write then read returns what went in ---");
{
    const b = new Bytes(new Uint8Array(16));
    const CASES = [
        ["Int8", -128], ["Uint8", 255], ["Int16BE", -32768],
        ["Uint16BE", 65535], ["Uint16LE", 65535],
        ["Int32BE", -2147483648], ["Int32LE", 2147483647],
        ["BigInt64BE", -9223372036854775808n], ["BigInt64LE", 123456789n],
        ["BigUint64BE", 18446744073709551615n],
    ];
    for (const [name, v] of CASES) {
        const wr = "write" + name, rd = "read" + name;
        if (typeof b[wr] !== "function" || typeof b[rd] !== "function") continue;
        b[wr](0, v);
        ok(b[rd](0) === v, name + " round trips at its extreme (" + v + ")", b[rd](0));
    }
    /* floats: exactly representable values only, so equality is meaningful */
    for (const name of ["FloatBE", "FloatLE"]) {
        if (typeof b["write" + name] !== "function") continue;
        b["write" + name](0, 0.5);
        ok(b["read" + name](0) === 0.5, name + " round trips 0.5");
        b["write" + name](0, -0);
        ok(Object.is(b["read" + name](0), -0), name + " preserves -0");
    }
}

print("--- Text: transcoders, against TextEncoder as the reference ---");
{
    /* These are INSTANCE methods, not statics. Calling them as statics is how
       an earlier draft of this file skipped the whole section in silence --
       every `typeof Text.foo === "function"` was false and nothing ran. */
    const need = ["isValidUtf8", "isValidUtf16", "countUtf8", "countUtf16",
                  "toUtf8", "toBytes", "latin1ToUtf8", "utf8ToLatin1",
                  "utf8ToUtf16", "utf16ToUtf8"];
    const proto = Object.getPrototypeOf(new Text("x"));
    for (const m of need)
        ok(typeof proto[m] === "function", "Text.prototype." + m + " exists");

    /* toUtf8 FORWARDED TO utf16ToUtf8 and returned garbage for every input --
       "ab" gave [230,137,161] and odd lengths threw. TextEncoder is the
       reference; a round trip against our own decoder would not have caught
       it, because both sides were consistent with each other. */
    for (const str of ["a", "ab", "abc", "héllo", "a→b", "😀", ""]) {
        const want = Array.from(new TextEncoder().encode(str)).join(",");
        let got;
        try { got = Array.from(new Text(str).toUtf8()).join(","); }
        catch (e) { got = "THREW " + String(e.message).slice(0, 30); }
        ok(got === want, "Text(" + JSON.stringify(str) + ").toUtf8() matches " +
           "TextEncoder", got);
    }
    /* An ODD-LENGTH string is the case that threw. Keep one explicitly: an
       even-length-only corpus would have missed half the fault. */
    ok(Array.from(new Text("abc").toUtf8()).length === 3,
       "an ODD-length string does not throw (it did: byte length must be even)");

    const t = new Text("héllo");
    ok(t.isValidUtf8() === true, "isValidUtf8 accepts valid text");
    ok(t.countUtf8() === 5, "countUtf8 counts code points", t.countUtf8());
    /* countUtf16 counts CODE POINTS -- "surrogate pairs count once", per its
       own comment -- NOT UTF-16 units. I asserted units first and it failed;
       the code was right. The assertion that actually pins the contract is
       that both counters AGREE, since both are code-point counts. */
    ok(new Text("a😀").countUtf16() === 2,
       "countUtf16 counts CODE POINTS (a surrogate pair counts once)",
       new Text("a😀").countUtf16());
    for (const str of ["a😀", "héllo", "a→b", ""])
        ok(new Text(str).countUtf8() === new Text(str).countUtf16(),
           "countUtf8 and countUtf16 agree on " + JSON.stringify(str) +
           " (both are code-point counts)");
    ok(new Text("a😀").countUtf16() !== "a😀".length,
       "and neither equals String.length, which IS units (3)");
    /* The UTF-16-input methods took the string's UTF-8 bytes, so ASCII was
       reported INVALID and counted at a third of its length. Both surrogate
       halves matter: a lone LOW surrogate is as ill-formed as a lone high one,
       and an implementation that only scans for highs passes the obvious probe. */
    ok(new Text("abc").isValidUtf16() === true,
       "isValidUtf16 accepts plain ASCII (it answered FALSE)");
    ok(new Text("a\uD800b").isValidUtf16() === false, "a lone HIGH surrogate is invalid");
    ok(new Text("a\uDFFFb").isValidUtf16() === false, "a lone LOW surrogate is invalid");
    ok(new Text("abc").countUtf16() === 3,
       "countUtf16 of ASCII is 3 (it answered 1)", new Text("abc").countUtf16());
    ok(Array.from(new Text("abc").utf16ToUtf8()).join(",") === "97,98,99",
       "utf16ToUtf8 on a Text is its UTF-8 encoding (it threw)");
    /* A LONE SURROGATE separates a real check from "are there any units". */
    ok(new Text("a\uD800b").isValidUtf16() === false,
       "isValidUtf16 REJECTS a lone high surrogate");
    ok(new Text("a😀b").isValidUtf16() === true,
       "and accepts a well-formed pair");
    ok(t.toBytes().length === 6, "toBytes is the UTF-8 encoding", t.toBytes().length);
}

print("test_bytes_accessors: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("test_bytes_accessors: " + fail + " failures");
