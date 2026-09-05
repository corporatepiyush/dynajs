/* WHATWG TextEncoder / TextDecoder (UTF-8) tests.
   Run: ./dynajs tests/test_textcodec.js  -> prints "ALL PASS" on success. */

function assert(actual, expected, message) {
    if (arguments.length === 1)
        expected = true;
    if (Object.is(actual, expected))
        return;
    throw Error("assertion failed: got |" + actual + "|, expected |" +
                expected + "|" + (message ? " (" + message + ")" : ""));
}

function assertBytes(actual, expected, message) {
    assert(actual instanceof Uint8Array, true, message + " (not Uint8Array)");
    assert(actual.length, expected.length, message + " (length)");
    for (let i = 0; i < expected.length; i++)
        assert(actual[i], expected[i], message + " (byte " + i + ")");
}

function assertThrows(errType, fn, message) {
    let threw = false;
    try {
        fn();
    } catch (e) {
        threw = e instanceof errType;
    }
    assert(threw, true, message);
}

const enc = new TextEncoder();
const dec = new TextDecoder();

function test_encoding_getters() {
    assert(new TextEncoder().encoding, "utf-8");
    assert(new TextDecoder().encoding, "utf-8");
    assert(new TextDecoder().fatal, false);
    assert(new TextDecoder().ignoreBOM, false);
    assert(new TextDecoder("utf-8", { fatal: true }).fatal, true);
    assert(new TextDecoder("utf-8", { ignoreBOM: true }).ignoreBOM, true);
}

function test_ascii_roundtrip() {
    const s = "Hello, World! 0123456789 ~!@#$%^&*()";
    const b = enc.encode(s);
    assert(b instanceof Uint8Array, true);
    assert(b.length, s.length);          // ASCII: 1 byte per char
    assert(dec.decode(b), s);
    assert(enc.encode("").length, 0);    // default input ""
    assert(enc.encode().length, 0);      // omitted argument
    assert(dec.decode(), "");            // omitted argument -> ""
}

function test_multibyte() {
    /* emoji U+1F600 (surrogate pair D83D DE00 in UTF-16) */
    const grin = "\u{1F600}";
    assert(grin.length, 2);              // two UTF-16 code units
    assertBytes(enc.encode(grin), [0xF0, 0x9F, 0x98, 0x80], "grin encode");
    assert(dec.decode(enc.encode(grin)), grin);

    /* CJK: U+4E2D U+6587 */
    const cjk = "中文";
    assertBytes(enc.encode(cjk),
                [0xE4, 0xB8, 0xAD, 0xE6, 0x96, 0x87], "cjk encode");
    assert(dec.decode(enc.encode(cjk)), cjk);

    /* combining mark: 'e' + combining acute accent U+0301 */
    const combo = "é";
    assert(combo.length, 2);
    assertBytes(enc.encode(combo), [0x65, 0xCC, 0x81], "combining encode");
    assert(dec.decode(enc.encode(combo)), combo);

    /* mixed string full round trip */
    const mixed = "aé中\u{1F600}z";
    assert(dec.decode(enc.encode(mixed)), mixed);
}

function test_encode_into() {
    /* "hello" needs 6 bytes (h + e-acute + l + l + o); dst of 3 fits h+e-acute */
    const dst = new Uint8Array(3);
    const r = enc.encodeInto("héllo", dst);
    assert(r.read, 2, "read");
    assert(r.written, 3, "written");
    assertBytes(dst, [0x68, 0xC3, 0xA9], "encodeInto partial");

    /* a 2-byte code point must NOT be split into a 1-byte destination */
    const dst1 = new Uint8Array(1);
    const r1 = enc.encodeInto("é", dst1);
    assert(r1.read, 0);
    assert(r1.written, 0);
    assert(dst1[0], 0, "dst1 untouched");

    /* a 4-byte emoji does not fit in 3 bytes: nothing written */
    const dst3 = new Uint8Array(3);
    const r3 = enc.encodeInto("\u{1F600}", dst3);
    assert(r3.read, 0);
    assert(r3.written, 0);

    /* exactly 4 bytes: emoji fits, read counts both surrogate halves */
    const dst4 = new Uint8Array(4);
    const r4 = enc.encodeInto("\u{1F600}", dst4);
    assert(r4.read, 2);
    assert(r4.written, 4);
    assertBytes(dst4, [0xF0, 0x9F, 0x98, 0x80], "encodeInto emoji exact");

    /* exact ASCII fit */
    const dst5 = new Uint8Array(5);
    const r5 = enc.encodeInto("ABCDE", dst5);
    assert(r5.read, 5);
    assert(r5.written, 5);

    /* roomy destination, boundary stops mid-string cleanly */
    const dst10 = new Uint8Array(10);
    const r6 = enc.encodeInto("A\u{1F600}B\u{1F600}", dst10); // 1+4+1+4 = 10
    assert(r6.read, 6);   // 1 + 2 + 1 + 2 UTF-16 code units
    assert(r6.written, 10);
}

function test_decode_sources() {
    const text = "café ☕ \u{1F600}";
    const src = enc.encode(text);

    /* Uint8Array */
    assert(dec.decode(src), text, "decode Uint8Array");
    /* ArrayBuffer */
    assert(dec.decode(src.buffer), text, "decode ArrayBuffer");

    /* offset subarray view into a padded buffer */
    const big = new Uint8Array(src.length + 4);
    big.set(src, 2);
    const view = big.subarray(2, 2 + src.length);
    assert(view.byteOffset, 2);
    assert(dec.decode(view), text, "decode subarray view");

    /* DataView over the same byte range */
    const dv = new DataView(big.buffer, 2, src.length);
    assert(dec.decode(dv), text, "decode DataView");

    /* Int8Array over the same bytes (still a BufferSource) */
    const i8 = new Int8Array(big.buffer, 2, src.length);
    assert(dec.decode(i8), text, "decode Int8Array");
}

function test_invalid_sequences() {
    const FFFD = "�";
    /* lone continuation / invalid lead bytes */
    assert(dec.decode(new Uint8Array([0x80])), FFFD);
    assert(dec.decode(new Uint8Array([0xFF])), FFFD);
    /* truncated 2-byte lead */
    assert(dec.decode(new Uint8Array([0xC3])), FFFD);
    /* valid ASCII surrounding an invalid byte */
    assert(dec.decode(new Uint8Array([0x41, 0xFF, 0x42])), "A" + FFFD + "B");
    /* E0 then 0x80 (below the 0xA0 lower bound): two errors */
    assert(dec.decode(new Uint8Array([0xE0, 0x80])), FFFD + FFFD);
    /* overlong '/' (C0 AF): both bytes are invalid leads */
    assert(dec.decode(new Uint8Array([0xC0, 0xAF])), FFFD + FFFD);
    /* truncated emoji (missing last continuation) -> single replacement */
    assert(dec.decode(new Uint8Array([0xF0, 0x9F, 0x98])), FFFD);
    /* a valid code point still decodes after recovery */
    assert(dec.decode(new Uint8Array([0xFF, 0x41])), FFFD + "A");
}

function test_fatal() {
    const fdec = new TextDecoder("utf-8", { fatal: true });
    assert(fdec.fatal, true);
    assertThrows(TypeError, () => fdec.decode(new Uint8Array([0xFF])),
                 "fatal lone 0xFF");
    assertThrows(TypeError, () => fdec.decode(new Uint8Array([0xE0, 0x80])),
                 "fatal invalid continuation");
    assertThrows(TypeError, () => fdec.decode(new Uint8Array([0xC3])),
                 "fatal truncated");
    /* valid input still decodes under fatal mode */
    assert(fdec.decode(enc.encode("ok \u{1F600}")), "ok \u{1F600}");
}

function test_bom() {
    const bom = [0xEF, 0xBB, 0xBF];
    const withBom = new Uint8Array(bom.concat([0x41, 0x42])); // BOM + "AB"

    /* default: leading BOM stripped */
    assert(new TextDecoder().decode(withBom), "AB");
    /* ignoreBOM: BOM kept as U+FEFF */
    const ib = new TextDecoder("utf-8", { ignoreBOM: true });
    assert(ib.decode(withBom), "﻿AB");
    /* only ONE leading BOM is stripped */
    const twoBom = new Uint8Array(bom.concat(bom).concat([0x41]));
    assert(new TextDecoder().decode(twoBom), "﻿A");
    /* a BOM in the middle is preserved */
    const midBom = new Uint8Array([0x41].concat(bom).concat([0x42]));
    assert(new TextDecoder().decode(midBom), "A﻿B");
}

function test_labels() {
    /* accepted labels: case-insensitive, whitespace-trimmed */
    assert(new TextDecoder("utf-8").encoding, "utf-8");
    assert(new TextDecoder("UTF-8").encoding, "utf-8");
    assert(new TextDecoder("utf8").encoding, "utf-8");
    assert(new TextDecoder("  Utf8  ").encoding, "utf-8");
    assert(new TextDecoder("unicode-1-1-utf-8").encoding, "utf-8");
    assert(new TextDecoder("\tUNICODE-1-1-UTF-8\n").encoding, "utf-8");
    assert(new TextDecoder().encoding, "utf-8");
    assert(new TextDecoder(undefined).encoding, "utf-8");

    /* rejected labels: RangeError */
    assertThrows(RangeError, () => new TextDecoder("utf-16"), "utf-16");
    assertThrows(RangeError, () => new TextDecoder("utf-16le"), "utf-16le");
    assertThrows(RangeError, () => new TextDecoder("latin1"), "latin1");
    assertThrows(RangeError, () => new TextDecoder("iso-8859-1"), "iso-8859-1");
    assertThrows(RangeError, () => new TextDecoder("ascii"), "ascii");
    assertThrows(RangeError, () => new TextDecoder(""), "empty label");
    assertThrows(RangeError, () => new TextDecoder("utf-8x"), "utf-8x");
}

function test_coercion() {
    /* encode coerces its argument via String() */
    assertBytes(enc.encode(123), [0x31, 0x32, 0x33], "encode number");
    assertBytes(enc.encode(null), [0x6E, 0x75, 0x6C, 0x6C], "encode null");
    /* encodeInto coerces its source too */
    const d = new Uint8Array(3);
    const r = enc.encodeInto(123, d);
    assert(r.read, 3);
    assert(r.written, 3);
    assertBytes(d, [0x31, 0x32, 0x33], "encodeInto number");
}

function test_reentrancy() {
    /* encodeInto: the source's toString() detaches the destination buffer
       while it is being coerced. The destination pointer is resolved only
       afterwards, so this must fail cleanly (never a use-after-free). */
    const dst = new Uint8Array(8);
    let detached = false;
    const evilSource = {
        toString() { dst.buffer.transfer(); detached = true; return "hello"; }
    };
    let handled = false;
    try {
        const r = enc.encodeInto(evilSource, dst);
        handled = (r.written === 0);
    } catch (e) {
        handled = e instanceof TypeError;
    }
    assert(detached, true, "evil source ran");
    assert(handled, true, "encodeInto survived detached destination");

    /* decode: a DataView-like object whose byteLength getter detaches the
       backing buffer during argument resolution. */
    const bytes = new Uint8Array([0x41, 0x42, 0x43]);
    const evilView = {
        get buffer() { return bytes.buffer; },
        get byteOffset() { return 0; },
        get byteLength() { bytes.buffer.transfer(); return 3; }
    };
    let safe = false;
    try {
        dec.decode(evilView);
        safe = true;                 // returning is fine, as long as no crash
    } catch (e) {
        safe = (e instanceof TypeError) || (e instanceof RangeError);
    }
    assert(safe, true, "decode survived detach during resolution");
}

function test_object_churn() {
    /* create and drop many short-lived instances to exercise the finalizers */
    for (let i = 0; i < 2000; i++) {
        const e = new TextEncoder();
        const d = new TextDecoder(i & 1 ? "utf-8" : "utf8", { fatal: false });
        const s = "churn-" + i + " é中\u{1F600}";
        if (d.decode(e.encode(s)) !== s)
            throw Error("churn mismatch at " + i);
    }
}

test_encoding_getters();
test_ascii_roundtrip();
test_multibyte();
test_encode_into();
test_decode_sources();
test_invalid_sequences();
test_fatal();
test_bom();
test_labels();
test_coercion();
test_reentrancy();
test_object_churn();

console.log("ALL PASS");
