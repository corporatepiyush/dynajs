// covers: integration c:src/modules/libc/libc-text-codec.c c:src/core/dyn-textdec.c -- the binding and the decoder core together
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_textcodec");
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
    assert(b.length, s.length);
    assert(dec.decode(b), s);
    assert(enc.encode("").length, 0);
    assert(enc.encode().length, 0);
    assert(dec.decode(), "");
}

function test_multibyte() {
    const grin = "\u{1F600}";
    assert(grin.length, 2);
    assertBytes(enc.encode(grin), [0xF0, 0x9F, 0x98, 0x80], "grin encode");
    assert(dec.decode(enc.encode(grin)), grin);

    const cjk = "中文";
    assertBytes(enc.encode(cjk),
                [0xE4, 0xB8, 0xAD, 0xE6, 0x96, 0x87], "cjk encode");
    assert(dec.decode(enc.encode(cjk)), cjk);

    const combo = "é";
    assert(combo.length, 2);
    assertBytes(enc.encode(combo), [0x65, 0xCC, 0x81], "combining encode");
    assert(dec.decode(enc.encode(combo)), combo);

    const mixed = "aé中\u{1F600}z";
    assert(dec.decode(enc.encode(mixed)), mixed);
}

function test_encode_into() {
    const dst = new Uint8Array(3);
    const r = enc.encodeInto("héllo", dst);
    assert(r.read, 2, "read");
    assert(r.written, 3, "written");
    assertBytes(dst, [0x68, 0xC3, 0xA9], "encodeInto partial");

    const dst1 = new Uint8Array(1);
    const r1 = enc.encodeInto("é", dst1);
    assert(r1.read, 0);
    assert(r1.written, 0);
    assert(dst1[0], 0, "dst1 untouched");

    const dst3 = new Uint8Array(3);
    const r3 = enc.encodeInto("\u{1F600}", dst3);
    assert(r3.read, 0);
    assert(r3.written, 0);

    const dst4 = new Uint8Array(4);
    const r4 = enc.encodeInto("\u{1F600}", dst4);
    assert(r4.read, 2);
    assert(r4.written, 4);
    assertBytes(dst4, [0xF0, 0x9F, 0x98, 0x80], "encodeInto emoji exact");

    const dst5 = new Uint8Array(5);
    const r5 = enc.encodeInto("ABCDE", dst5);
    assert(r5.read, 5);
    assert(r5.written, 5);

    const dst10 = new Uint8Array(10);
    const r6 = enc.encodeInto("A\u{1F600}B\u{1F600}", dst10);
    assert(r6.read, 6);
    assert(r6.written, 10);
}

function test_decode_sources() {
    const text = "café ☕ \u{1F600}";
    const src = enc.encode(text);

    assert(dec.decode(src), text, "decode Uint8Array");
    assert(dec.decode(src.buffer), text, "decode ArrayBuffer");

    const big = new Uint8Array(src.length + 4);
    big.set(src, 2);
    const view = big.subarray(2, 2 + src.length);
    assert(view.byteOffset, 2);
    assert(dec.decode(view), text, "decode subarray view");

    const dv = new DataView(big.buffer, 2, src.length);
    assert(dec.decode(dv), text, "decode DataView");

    const i8 = new Int8Array(big.buffer, 2, src.length);
    assert(dec.decode(i8), text, "decode Int8Array");
}

function test_invalid_sequences() {
    const FFFD = "�";
    assert(dec.decode(new Uint8Array([0x80])), FFFD);
    assert(dec.decode(new Uint8Array([0xFF])), FFFD);
    assert(dec.decode(new Uint8Array([0xC3])), FFFD);
    assert(dec.decode(new Uint8Array([0x41, 0xFF, 0x42])), "A" + FFFD + "B");
    assert(dec.decode(new Uint8Array([0xE0, 0x80])), FFFD + FFFD);
    assert(dec.decode(new Uint8Array([0xC0, 0xAF])), FFFD + FFFD);
    assert(dec.decode(new Uint8Array([0xF0, 0x9F, 0x98])), FFFD);
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
    assert(fdec.decode(enc.encode("ok \u{1F600}")), "ok \u{1F600}");
}

function test_bom() {
    const bom = [0xEF, 0xBB, 0xBF];
    const withBom = new Uint8Array(bom.concat([0x41, 0x42]));

    assert(new TextDecoder().decode(withBom), "AB");
    const ib = new TextDecoder("utf-8", { ignoreBOM: true });
    assert(ib.decode(withBom), "﻿AB");
    const twoBom = new Uint8Array(bom.concat(bom).concat([0x41]));
    assert(new TextDecoder().decode(twoBom), "﻿A");
    const midBom = new Uint8Array([0x41].concat(bom).concat([0x42]));
    assert(new TextDecoder().decode(midBom), "A﻿B");
}

function test_labels() {
    assert(new TextDecoder("utf-8").encoding, "utf-8");
    assert(new TextDecoder("UTF-8").encoding, "utf-8");
    assert(new TextDecoder("utf8").encoding, "utf-8");
    assert(new TextDecoder("  Utf8  ").encoding, "utf-8");
    assert(new TextDecoder("unicode-1-1-utf-8").encoding, "utf-8");
    assert(new TextDecoder("\tUNICODE-1-1-UTF-8\n").encoding, "utf-8");
    assert(new TextDecoder().encoding, "utf-8");
    assert(new TextDecoder(undefined).encoding, "utf-8");

    assert(new TextDecoder("utf-16").encoding, "utf-16le");
    assert(new TextDecoder("UTF-16BE").encoding, "utf-16be");
    assert(new TextDecoder("koi8-r").encoding, "koi8-r");
    assert(new TextDecoder("latin2").encoding, "iso-8859-2");
    assert(new TextDecoder("sjis").encoding, "shift_jis");
    assert(new TextDecoder("gb2312").encoding, "gbk");
    for (const l of ["replacement", "iso-2022-kr", "hz-gb-2312", "csiso2022kr"])
        assertThrows(RangeError, () => new TextDecoder(l), "replacement label " + l);
    assertThrows(RangeError, () => new TextDecoder(""), "empty label");
    assertThrows(RangeError, () => new TextDecoder("utf-8x"), "utf-8x");
}

// WHATWG Encoding: latin1, iso-8859-1, ascii and the other windows-1252 labels all decode as windows-1252
// (perf observation O16: they used to throw RangeError, leaving no byte -> string route but utf-8).
// Index table: https://encoding.spec.whatwg.org/index-windows-1252.txt (0x80-0x9F; the rest is identity).
// WHATWG Encoding legacy decoders (src/core/dyn-textdec.c). One pinned vector per index branch that the
// WPT run does not reach (its legacy-mb-* japanese/korean/tchinese suites are HTML-only): big5 two-code-point
// pointers 1133/1135, euc-jp 0x8E katakana and 0x8F jis0212, shift_jis pointers 8836+ to the PUA,
// x-user-defined 0xF780 base, gb18030 four-byte ranges. Values cross-checked with iconv where it agrees with WHATWG.
function test_legacy_multibyte() {
    const cps = (e, b, o) => [...new TextDecoder(e, o).decode(new Uint8Array(b))].map((c) => c.codePointAt(0).toString(16)).join(" ");
    assert(cps("big5", [0x88, 0x62, 0x88, 0x64, 0xA4, 0x40]), "ca 304 ca 30c 4e00", "big5");
    assert(cps("euc-jp", [0x8F, 0xA2, 0xAF, 0x8E, 0xB1, 0xA4, 0xA2]), "2d8 ff71 3042", "euc-jp");
    assert(cps("shift_jis", [0xF0, 0x40, 0x82, 0xA0, 0xB1]), "e000 3042 ff71", "shift_jis");
    assert(cps("x-user-defined", [0x41, 0x80, 0xFF]), "41 f780 f7ff", "x-user-defined");
    assert(cps("gb18030", [0x81, 0x30, 0x81, 0x30, 0xA8, 0xBF, 0x90, 0x30, 0x81, 0x30, 0x84, 0x31, 0xA4, 0x39]), "80 1f9 10000 ffff", "gb18030");
    assert(cps("gbk", [0x80, 0xC4, 0xE3]), "20ac 4f60", "gbk");
    assert(cps("gb18030", [0x81, 0x35, 0xF4, 0x37]), "e7c7", "gb18030 pointer 7457 special case");
    assert(cps("iso-2022-jp", [0x1B, 0x28, 0x42, 0x1B, 0x28, 0x42, 0x41]), "fffd 41", "iso-2022-jp back-to-back escapes (output flag)");
    assert(cps("euc-kr", [0xB0, 0xA1]), "ac00", "euc-kr");
    assert(cps("iso-2022-jp", [0x1B, 0x24, 0x42, 0x24, 0x22, 0x1B, 0x28, 0x42, 0x41]), "3042 41", "iso-2022-jp");
    assert(cps("koi8-r", [0xC1]), "430", "koi8-r");
    assert(cps("utf-16le", [0xFF, 0xFE, 0x41, 0x00, 0x3D, 0xD8, 0x00, 0xDE]), "41 1f600", "utf-16le bom + pair");
    assert(cps("utf-16be", [0x00, 0x41, 0xD8]), "41 fffd", "utf-16be odd tail");
    assertThrows(TypeError, () => new TextDecoder("utf-16le", { fatal: true }).decode(new Uint8Array([0x41])), "utf-16 fatal unterminated");
    assertThrows(TypeError, () => new TextDecoder("utf-16le", { fatal: true }).decode(new Uint8Array([0x3D, 0xD8])), "utf-16 fatal lone lead surrogate");
    for (const [e, b, lit] of [["shift_jis", [0x41, 0x42], "AB"], ["iso-8859-2", [0x41, 0xE9], "A\u00e9"],
        ["shift_jis", [0x41, 0x82, 0xA0], "A\u3042"], ["utf-16le", [0x3D, 0xD8, 0x00, 0xDE], "\u{1F600}"]]) {
        const s = new TextDecoder(e).decode(new Uint8Array(b));
        assert(s === lit, true, e + " result === literal (narrow when Latin-1, wide otherwise)");
        assert(new Map([[lit, 1]]).get(s), 1, e + " result as Map key");
        assert(({ [lit]: 2 })[s], 2, e + " result as property key");
        assert(JSON.stringify({ [s]: 0 }), JSON.stringify({ [lit]: 0 }), e + " result as created key");
    }
    const sj = new TextDecoder("shift_jis");
    assert(sj.decode(new Uint8Array([0x82]), { stream: true }) + sj.decode(new Uint8Array([0xA0])), "\u3042", "shift_jis streaming split");
    const big = new Uint8Array(200000).fill(0x41);
    big.set([0x82, 0xA0], 131071);
    const out = new TextDecoder("shift_jis").decode(big);
    assert(out.length, 199999, "chunk boundary split keeps the pair");
    assert(out.charCodeAt(131071), 0x3042, "pair across the 64 KiB chunk edge");
}

function test_windows1252() {
    const labels = ["ansi_x3.4-1968", "ascii", "cp1252", "cp819", "csisolatin1", "ibm819", "iso-8859-1",
        "iso-ir-100", "iso8859-1", "iso88591", "iso_8859-1", "iso_8859-1:1987", "l1", "latin1", "us-ascii",
        "windows-1252", "x-cp1252", " LATIN1\n", "US-ASCII"];
    for (const l of labels)
        assert(new TextDecoder(l).encoding, "windows-1252", "label " + JSON.stringify(l));
    const c1 = [0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039,
        0x0152, 0x008D, 0x017D, 0x008F, 0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC,
        0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178];
    const all = new Uint8Array(256);
    for (let i = 0; i < 256; i++) all[i] = i;
    const s = new TextDecoder("latin1").decode(all);
    assert(s.length, 256, "one code unit per byte");
    for (let i = 0; i < 256; i++) {
        const want = i >= 0x80 && i < 0xA0 ? c1[i - 0x80] : i;
        assert(s.charCodeAt(i), want, "byte 0x" + i.toString(16));
    }
    const f = new TextDecoder("windows-1252", { fatal: true });
    assert(f.decode(new Uint8Array([0x81, 0xFF])), "\u0081\u00FF", "fatal never fires: every byte maps");
    assert(new TextDecoder("latin1").decode(new Uint8Array([0xEF, 0xBB, 0xBF, 0x41])), "\u00EF\u00BB\u00BFA",
        "no BOM is stripped: EF BB BF are three characters");
    assert(new TextDecoder("ascii").decode(new Uint8Array([104, 105])), "hi", "ASCII bytes");
    assert(new TextDecoder("ascii").decode(), "", "no argument decodes to the empty string");
    assert(new TextDecoder("latin1").decode(new Uint8Array([0xE9]).buffer), "\u00E9", "an ArrayBuffer source");
    assert(new TextDecoder("latin1").decode(new Uint8Array([0x41, 0xE9]), { stream: true }), "A\u00E9",
        "stream is accepted; a single-byte decoder carries nothing");
    assertThrows(TypeError, () => new TextDecoder("latin1").decode(new Uint8Array(1), { stream: 1 }),
        "stream must still be a boolean");
}

function test_coercion() {
    assertBytes(enc.encode(123), [0x31, 0x32, 0x33], "encode number");
    assertBytes(enc.encode(null), [0x6E, 0x75, 0x6C, 0x6C], "encode null");
    const d = new Uint8Array(3);
    const r = enc.encodeInto(123, d);
    assert(r.read, 3);
    assert(r.written, 3);
    assertBytes(d, [0x31, 0x32, 0x33], "encodeInto number");
}

function test_reentrancy() {
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

    const bytes = new Uint8Array([0x41, 0x42, 0x43]);
    const evilView = {
        get buffer() { return bytes.buffer; },
        get byteOffset() { return 0; },
        get byteLength() { bytes.buffer.transfer(); return 3; }
    };
    let safe = false;
    try {
        dec.decode(evilView);
        safe = true;
    } catch (e) {
        safe = (e instanceof TypeError) || (e instanceof RangeError);
    }
    assert(safe, true, "decode survived detach during resolution");
}

function test_object_churn() {
    for (let i = 0; i < 2000; i++) {
        const e = new TextEncoder();
        const d = new TextDecoder(i & 1 ? "utf-8" : "utf8", { fatal: false });
        const s = "churn-" + i + " é中\u{1F600}";
        if (d.decode(e.encode(s)) !== s)
            throw Error("churn mismatch at " + i);
    }
}

__kitS.section("test_encoding_getters", () => test_encoding_getters());
__kitS.section("test_ascii_roundtrip", () => test_ascii_roundtrip());
__kitS.section("test_multibyte", () => test_multibyte());
__kitS.section("test_encode_into", () => test_encode_into());
__kitS.section("test_decode_sources", () => test_decode_sources());
__kitS.section("test_invalid_sequences", () => test_invalid_sequences());
__kitS.section("test_fatal", () => test_fatal());
__kitS.section("test_bom", () => test_bom());
__kitS.section("test_labels", () => test_labels());
__kitS.section("test_legacy_multibyte", () => test_legacy_multibyte());
__kitS.section("test_windows1252", () => test_windows1252());
__kitS.section("test_coercion", () => test_coercion());
__kitS.section("test_reentrancy", () => test_reentrancy());
__kitS.section("test_object_churn", () => test_object_churn());
__kitS.end();

console.log("ALL PASS");
