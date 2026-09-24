/* WHATWG TextDecoder streaming ({stream:true}) + strict decode/ctor
 options.
   Run: ./dynajs tests/test_textdecoder_stream.js -> prints "ALL PASS".

   Sections 1a-1c are the COMPLETE 38-row byte-identity corpus (scratch/
   probe_corpus.js): every row's expected output is a LITERAL captured from
   the pristine pre-streaming binary (30 non-stream rows + 6 fatal rows +
 2 ignoreBOM rows). must not change a single byte of stateless
   decode(); this corpus is the permanent regression for that contract.
   Streaming expectations follow the WHATWG algorithm as implemented by
   node v22 (oracle-verified, 54-case differential). */

function assert(actual, expected, message) {
    if (arguments.length === 1)
        expected = true;
    if (Object.is(actual, expected))
        return;
    throw Error("assertion failed: got |" + actual + "|, expected |" +
                expected + "|" + (message ? " (" + message + ")" : ""));
}

function assertThrowsMsg(errType, msg, fn, message) {
    var threw = false, m;
    try {
        fn();
    } catch (e) {
        threw = e instanceof errType;
        m = e.message;
    }
    assert(threw, true, message + " (wrong or no throw)");
    assert(m, msg, message + " (wrong message)");
}

function u8(arr) { return new Uint8Array(arr); }
var FFFD = "\uFFFD";

/* --- 1. non-stream byte-identity: literals from the pristine binary ------ */

var NONSTREAM = [
    [[0x68, 0x69], "hi"],
    [[], ""],
    [[0xEF, 0xBB, 0xBF, 0x61], "a"],
    [[0xEF, 0xBB, 0xBF], ""],
    [[0xEF, 0xBB, 0xBF, 0x61, 0xEF, 0xBB, 0xBF], "a\uFEFF"],
    [[0xC3], FFFD],
    [[0xE2, 0x82], FFFD],
    [[0xF0, 0x9F, 0x98], FFFD],
    [[0xF0, 0x9F, 0x98, 0x80], "\uD83D\uDE00"],
    [[0xC3, 0xA9], "\u00E9"],
    [[0xFF], FFFD],
    [[0x80], FFFD],
    [[0xC0, 0xAF], FFFD + FFFD],
    [[0xE0, 0x80, 0xAF], FFFD + FFFD + FFFD],
    [[0xED, 0xA0, 0x80], FFFD + FFFD + FFFD],
    [[0xF4, 0x90, 0x80, 0x80], FFFD + FFFD + FFFD + FFFD],
    [[0xF5, 0x80, 0x80, 0x80], FFFD + FFFD + FFFD + FFFD],
    [[0xE0, 0x9F, 0xBF], FFFD + FFFD + FFFD],
    [[0xF4, 0x8F, 0xBF, 0xBF], "\uDBFF\uDFFF"],
    [[0xC2], FFFD],
    [[0x61, 0xFF, 0x62], "a" + FFFD + "b"],
    [[0x61, 0xC3, 0x28, 0x62], "a" + FFFD + "(b"],
    [[0xE2, 0x82, 0xAC], "\u20AC"],
    [[0xF0, 0x9F, 0x92, 0xA9, 0xF0, 0x9F, 0x98, 0x80],
     "\uD83D\uDCA9\uD83D\uDE00"],
    [[0xC3, 0xA9, 0x0A, 0xE2, 0x82, 0xAC, 0xF0, 0x9F, 0x98, 0x80],
     "\u00E9\n\u20AC\uD83D\uDE00"],
    [[0xFF, 0xEF, 0xBB, 0xBF], FFFD + "\uFEFF"],
    [[0xEF], FFFD],
    [[0xC2, 0x28], FFFD + "("],
    [[0xE0, 0x41], FFFD + "A"],
    [[0x7F], "\u007F"],
];

function test_nonstream_identity() {
    for (var i = 0; i < NONSTREAM.length; i++) {
        assert(new TextDecoder().decode(u8(NONSTREAM[i][0])), NONSTREAM[i][1],
               "non-stream corpus row " + i);
    }
    /* repeated decode() with no args: fresh decoder, always "" */
    var d = new TextDecoder();
    assert(d.decode(), "", "decode() no args");
    assert(d.decode(), "", "decode() no args x2");
    assert(d.decode(undefined), "", "decode(undefined)");
    assert(d.decode(undefined, { stream: false }), "",
           "decode(undefined, {stream:false})");
    /* per-call BOM strip on plain calls (pre-streaming behavior) */
    var p = new TextDecoder();
    assert(p.decode(u8([0xEF, 0xBB, 0xBF, 0x61])), "a", "plain BOM 1");
    assert(p.decode(u8([0xEF, 0xBB, 0xBF, 0x62])), "b", "plain BOM 2");
}

function test_nonstream_fatal_identity() {
    var throws = [
        [[0xFF], "ff"],
        [[0xC3], "trailing c3"],
        [[0xC0, 0xAF], "overlong"],
        [[0xED, 0xA0, 0x80], "surrogate"],
        [[0xE0, 0x41], "e0 then A"],
    ];
    for (var i = 0; i < throws.length; i++) {
        var err = null;
        try {
            new TextDecoder("utf-8", { fatal: true })
                .decode(u8(throws[i][0]));
        } catch (e) {
            err = e;
        }
        assert(err instanceof TypeError, true,
               "fatal throw " + throws[i][1]);
    }
    assert(new TextDecoder("utf-8", { fatal: true })
               .decode(u8([0xC3, 0xA9])), "\u00E9", "fatal valid");
}

function test_ignoreBOM_identity() {
    assert(new TextDecoder("utf-8", { ignoreBOM: true })
               .decode(u8([0xEF, 0xBB, 0xBF, 0x61])), "\uFEFFa",
           "ignoreBOM keeps U+FEFF");
    assert(new TextDecoder("utf-8", { ignoreBOM: true, fatal: true })
               .decode(u8([0xEF, 0xBB, 0xBF])), "\uFEFF",
           "ignoreBOM+fatal keeps U+FEFF");
    /* getters unchanged */
    var g = new TextDecoder("utf-8", { fatal: false, ignoreBOM: true });
    assert(g.encoding, "utf-8");
    assert(g.fatal, false);
    assert(g.ignoreBOM, true);
}

/* --- 2. streaming: carry, flush, reset (WHATWG, node-v22-verified) -------- */

function test_stream_carry() {
    /* 4-byte emoji split 1+3, 2+2, 3+1 */
    var d13 = new TextDecoder();
    assert(d13.decode(u8([0xF0]), { stream: true }), "", "emoji 1/4 carry");
    assert(d13.decode(u8([0x9F, 0x98, 0x80]), { stream: false }),
           "\uD83D\uDE00", "emoji split 1+3");
    var d22 = new TextDecoder();
    assert(d22.decode(u8([0xF0, 0x9F]), { stream: true }), "", "emoji 2/4 carry");
    assert(d22.decode(u8([0x98, 0x80]), { stream: false }),
           "\uD83D\uDE00", "emoji split 2+2");
    var d31 = new TextDecoder();
    assert(d31.decode(u8([0xF0, 0x9F, 0x98]), { stream: true }), "",
           "emoji 3/4 carry");
    assert(d31.decode(u8([0x80]), { stream: false }),
           "\uD83D\uDE00", "emoji split 3+1");
    /* 2-byte é split 1+1 */
    var de = new TextDecoder();
    assert(de.decode(u8([0xC3]), { stream: true }), "", "e-acute carry");
    assert(de.decode(u8([0xA9]), { stream: false }), "\u00E9", "e split 1+1");
    /* 3-byte € carried across TWO stream calls, flushed by a final decode() */
    var dur = new TextDecoder();
    assert(dur.decode(u8([0xE2, 0x82]), { stream: true }), "", "euro carry 1");
    assert(dur.decode(u8([0xAC]), { stream: true }), "\u20AC",
           "euro completes on stream call");
    assert(dur.decode(), "", "flush after complete sequence");
    /* carry held across a no-input stream:true call mid-stream */
    var dm = new TextDecoder();
    assert(dm.decode(u8([0xE2, 0x82]), { stream: true }), "", "mid carry");
    assert(dm.decode(undefined, { stream: true }), "",
           "no-input stream:true keeps state");
    assert(dm.decode(u8([0xAC]), { stream: true }), "\u20AC",
           "carry survives no-input call");
    assert(dm.decode(), "", "final flush");
    /* every split of a mixed multi-codepoint string round-trips */
    var s = "a\u00E9\u20AC\uD83D\uDE00b";
    var b = new TextEncoder().encode(s);
    for (var cut = 1; cut < b.length; cut++) {
        var ds = new TextDecoder();
        assert(ds.decode(b.subarray(0, cut), { stream: true }) +
               ds.decode(b.subarray(cut), { stream: false }),
               s, "round-trip split at " + cut);
    }
    /* 3-way split of the 4-byte emoji */
    var e4 = new TextEncoder().encode("\uD83D\uDE00");
    for (var i = 1; i < 4; i++) {
        for (var j = i + 1; j < 4; j++) {
            var d3 = new TextDecoder();
            assert(d3.decode(e4.subarray(0, i), { stream: true }) +
                   d3.decode(e4.subarray(i, j), { stream: true }) +
                   d3.decode(e4.subarray(j), { stream: false }),
                   "\uD83D\uDE00", "emoji 3-way split " + i + "/" + j);
        }
    }
}

function test_stream_flush() {
    /* incomplete final sequence flushed by decode() with no args */
    var d = new TextDecoder();
    d.decode(u8([0xC3]), { stream: true });
    assert(d.decode(), FFFD, "flush emits U+FFFD for carry");
    assert(d.decode(), "", "decoder usable after flush");
    /* flush folds carry into the next chunk */
    var d2 = new TextDecoder();
    assert(d2.decode(u8([0xC3, 0xA9, 0xC3]), { stream: true }), "\u00E9",
           "stream chunk decodes completed prefix");
    assert(d2.decode(u8([0xA9]), { stream: false }), "\u00E9",
           "final call consumes carry");
    /* {stream:false} explicitly is a final call too */
    var d3 = new TextDecoder();
    d3.decode(u8([0xE2, 0x82]), { stream: true });
    assert(d3.decode(u8([]), { stream: false }), FFFD,
           "empty final input still flushes carry");
    /* a no-input stream:true call on a FRESH decoder DOES open the stream
       (stream_active latches) but consumes nothing -- so the next chunk's
       leading BOM is still the stream head and gets stripped, by the
       OUTPUT-side sniff (the input-side strip is gated on a fresh stream) */
    var d4 = new TextDecoder();
    assert(d4.decode(undefined, { stream: true }), "", "no-input stream:true");
    assert(d4.decode(u8([0xEF, 0xBB, 0xBF, 0x61]), { stream: true }), "a",
           "BOM stripped after dormant stream:true");
}

function test_stream_bom() {
    /* BOM stripped once at stream start */
    var d = new TextDecoder();
    assert(d.decode(u8([0xEF, 0xBB, 0xBF, 0x61]), { stream: true }), "a",
           "stream head BOM");
    /* BOM-only opening chunk: consuming the head BOM latches BOM-seen even
       with no other output, so the NEXT chunk's BOM is U+FEFF content
       (node v22 oracle; regression for the double-strip bug) */
    var d8 = new TextDecoder();
    assert(d8.decode(u8([0xEF, 0xBB, 0xBF]), { stream: true }), "",
           "BOM-only opening chunk");
    assert(d8.decode(u8([0xEF, 0xBB, 0xBF, 0x61]), { stream: true }),
           "\uFEFFa", "second BOM is content (2-chunk)");
    /* three BOM-only chunks: "" | "\uFEFF" | "\uFEFFa" (node v22) */
    var d9 = new TextDecoder();
    assert(d9.decode(u8([0xEF, 0xBB, 0xBF]), { stream: true }), "",
           "BOM-only chunk 1");
    assert(d9.decode(u8([0xEF, 0xBB, 0xBF]), { stream: true }), "\uFEFF",
           "BOM-only chunk 2 is content");
    assert(d9.decode(u8([0xEF, 0xBB, 0xBF, 0x61]), { stream: true }),
           "\uFEFFa", "BOM-only chunk 3 is content");
    /* ...and when the final call carries the second BOM */
    var d10 = new TextDecoder();
    assert(d10.decode(u8([0xEF, 0xBB, 0xBF]), { stream: true }), "",
           "BOM-only opening (final variant)");
    assert(d10.decode(u8([0xEF, 0xBB, 0xBF, 0x61])), "\uFEFFa",
           "second BOM content on final call");
    /* BOM completing in a LATER chunk than the stream opener is still
       stripped (WHATWG: once per stream) */
    var d2 = new TextDecoder();
    assert(d2.decode(u8([0xEF]), { stream: true }), "", "lone EF carried");
    assert(d2.decode(u8([0xBB, 0xBF, 0x61]), { stream: false }), "a",
           "split BOM stripped on completion");
    var d2b = new TextDecoder();
    d2b.decode(u8([0xEF]), { stream: true });
    assert(d2b.decode(u8([0xBB, 0xBF]), { stream: false }), "",
           "split BOM alone -> empty");
    /* a BOM at a LATER chunk boundary is content, not a strip */
    var d3 = new TextDecoder();
    assert(d3.decode(u8([0x61]), { stream: true }), "a");
    assert(d3.decode(u8([0xEF, 0xBB, 0xBF, 0x62]), { stream: true }),
           "\uFEFFb", "mid-stream BOM kept");
    /* ...including after a BOM was already stripped at stream start */
    var d3b = new TextDecoder();
    assert(d3b.decode(u8([0xEF, 0xBB, 0xBF, 0x61, 0xEF, 0xBB, 0xBF]),
                      { stream: true }), "a\uFEFF", "BOM a BOM");
    /* BOM already handled when invalid bytes opened the stream */
    var d4 = new TextDecoder();
    assert(d4.decode(u8([0xFF]), { stream: true }), FFFD, "FF opens stream");
    assert(d4.decode(u8([0xEF, 0xBB, 0xBF]), { stream: false }), "\uFEFF",
           "BOM after error is content");
    /* a final call RESETS: the next stream:true call sniffs a BOM again */
    var d5 = new TextDecoder();
    d5.decode(u8([0xEF]), { stream: true });
    assert(d5.decode(u8([0xBB, 0xBF, 0x61]), { stream: false }), "a",
           "split BOM then final");
    assert(d5.decode(u8([0xEF, 0xBB, 0xBF, 0x62]), { stream: true }), "b",
           "BOM re-sniffed on the new stream");
    /* mixed usage: stream:true then a plain final call does NOT re-strip
       (the stream is still open until the final call) -- node-verified */
    var d6 = new TextDecoder();
    assert(d6.decode(u8([0xEF, 0xBB, 0xBF, 0x61]), { stream: true }), "a",
           "stream BOM");
    assert(d6.decode(u8([0xEF, 0xBB, 0xBF, 0x62])), "\uFEFFb",
           "open stream: second BOM is content");
    /* two plain calls still strip per-call (pre-streaming behavior) */
    var d7 = new TextDecoder();
    assert(d7.decode(u8([0xEF, 0xBB, 0xBF, 0x61])), "a", "plain 1");
    assert(d7.decode(u8([0xEF, 0xBB, 0xBF, 0x62])), "b", "plain 2");
}

function test_stream_fatal() {
    /* carry is fine under fatal; only the FINAL call judges an incomplete
       tail */
    var d = new TextDecoder("utf-8", { fatal: true });
    assert(d.decode(u8([0xC3]), { stream: true }), "", "fatal carry ok");
    var threw = null;
    try {
        d.decode();
    } catch (e) {
        threw = e;
    }
    assert(threw instanceof TypeError, true, "fatal flush throws");
    /* the throw must not poison the decoder */
    assert(d.decode(new TextEncoder().encode("ok")), "ok",
           "fatal decoder recovers after throw");
    /* fatal + invalid byte mid-stream throws and recovers */
    var d2 = new TextDecoder("utf-8", { fatal: true });
    d2.decode(u8([0x61]), { stream: true });
    threw = null;
    try {
        d2.decode(u8([0xFF, 0x62]), { stream: true });
    } catch (e) {
        threw = e;
    }
    assert(threw instanceof TypeError, true, "fatal mid-stream throw");
    assert(d2.decode(u8([0x63]), { stream: true }), "c",
           "fatal decoder recovers mid-stream");
    /* fatal + a COMPLETE multi-byte split is fine */
    var d3 = new TextDecoder("utf-8", { fatal: true });
    assert(d3.decode(u8([0xF0, 0x9F, 0x98]), { stream: true }), "",
           "fatal emoji carry");
    assert(d3.decode(u8([0x80]), { stream: false }), "\uD83D\uDE00",
           "fatal split completes");
}

function test_stream_reset_and_isolation() {
    /* two decoders carry independently, interleaved */
    var d1 = new TextDecoder(), d2 = new TextDecoder();
    assert(d1.decode(u8([0xC3]), { stream: true }), "", "d1 carry");
    assert(d2.decode(u8([0xE2, 0x82]), { stream: true }), "", "d2 carry");
    assert(d1.decode(u8([0xA9]), { stream: true }), "\u00E9", "d1 done");
    assert(d2.decode(u8([0xAC]), { stream: true }), "\u20AC", "d2 done");
    assert(d1.decode(), "", "d1 flush");
    assert(d2.decode(), "", "d2 flush");
    /* final call resets: decoder fully reusable as a fresh one */
    var d3 = new TextDecoder();
    d3.decode(u8([0xF0, 0x9F, 0x98]), { stream: true });
    d3.decode(u8([0x80]), { stream: false });
    assert(d3.decode(u8([0xEF, 0xBB, 0xBF, 0x61]), { stream: true }), "a",
           "reused decoder sniffs BOM");
    /* a wrong-type option thrown mid-stream must leave state intact */
    var d4 = new TextDecoder();
    d4.decode(u8([0xC3]), { stream: true });
    var threw = null;
    try {
        d4.decode(u8([0x61]), { stream: "yes" });
    } catch (e) {
        threw = e;
    }
    assert(threw instanceof TypeError, true, "bad stream type throws");
    assert(d4.decode(), FFFD, "carry intact after bad options");
}

function test_stream_views() {
    var ab = new ArrayBuffer(6);
    var full = new Uint8Array(ab);
    full.set([0xC3, 0xA9, 0xE2, 0x82, 0xAC, 0x62]);
    var d = new TextDecoder();
    assert(d.decode(new Uint8Array(ab, 0, 1), { stream: true }), "",
           "typed-array view with offset");
    assert(d.decode(new DataView(ab, 1, 5), { stream: false }),
           "\u00E9\u20ACb", "DataView view consumed");
}

/* --- 3. strict options ---------------------------------------------- */

function test_strict_decode_options() {
    var d = new TextDecoder();
    assertThrowsMsg(TypeError, 'unknown option "strem" (valid: stream)',
                    function () { d.decode(u8([0x61]), { strem: 1 }); },
                    "unknown decode option rejected");
    assertThrowsMsg(TypeError, 'unknown option "Stream" (valid: stream)',
                    function () { d.decode(u8([0x61]), { Stream: true }); },
                    "case-sensitive keys");
    assertThrowsMsg(TypeError, "decode() options must be an object",
                    function () { d.decode(u8([0x61]), "stream"); },
                    "string options rejected");
    assertThrowsMsg(TypeError, "decode() options must be an object",
                    function () { d.decode(u8([0x61]), 7); },
                    "number options rejected");
    assertThrowsMsg(TypeError, 'option "stream" must be a boolean',
                    function () { d.decode(u8([0x61]), { stream: "yes" }); },
                    "wrong stream type rejected");
    assertThrowsMsg(TypeError, 'option "stream" must be a boolean',
                    function () { d.decode(u8([0x61]), { stream: 1 }); },
                    "numeric stream rejected");
    /* accepted shapes */
    assert(d.decode(u8([0x61])), "a", "no options");
    assert(d.decode(u8([0x61]), undefined), "a", "undefined options");
    assert(d.decode(u8([0x61]), null), "a", "null options");
    assert(d.decode(u8([0x61]), {}), "a", "empty options");
    assert(d.decode(u8([0x61]), { stream: undefined }), "a",
           "undefined stream");
    assert(d.decode(u8([0x61]), { stream: false }), "a", "stream false");
    assert(d.decode(u8([0x61]), { stream: true }).length >= 0, true,
           "stream true");
    /* a rejected options bag must not disturb the decoder */
    var d2 = new TextDecoder();
    d2.decode(u8([0xC3]), { stream: true });
    try {
        d2.decode(u8([0x61]), { bogus: 1 });
    } catch (e) {}
    assert(d2.decode(u8([0xA9]), { stream: false }), "\u00E9",
           "state intact after rejected options");
}

function test_strict_ctor_options() {
    assertThrowsMsg(TypeError,
                    'unknown option "fatl" (valid: fatal, ignoreBOM)',
                    function () { new TextDecoder("utf-8", { fatl: true }); },
                    "unknown ctor option rejected");
    assertThrowsMsg(TypeError,
                    'unknown option "extra" (valid: fatal, ignoreBOM)',
                    function () {
                        new TextDecoder("utf-8", { ignoreBOM: true, extra: 1 });
                    },
                    "valid+unknown ctor options rejected");
    assertThrowsMsg(TypeError, "TextDecoder options must be an object",
                    function () { new TextDecoder("utf-8", "fatal"); },
                    "string ctor options rejected");
    /* valid shapes unchanged */
    var ok = new TextDecoder("utf-8", { fatal: true, ignoreBOM: true });
    assert(ok.fatal, true, "valid ctor options: fatal");
    assert(ok.ignoreBOM, true, "valid ctor options: ignoreBOM");
    assert(new TextDecoder("utf-8", null).encoding, "utf-8",
           "null ctor options");
    assert(new TextDecoder("utf8").encoding, "utf-8", "label utf8");
}

function test_perf_sanity() {
    /* 100 KB of ASCII through the non-stream hot path: correctness only
       (the measured A/B lives in the CHANGELOG) */
    var line = "abcdefghijklmnopqrstuvwxyz0123456789";
    var s = new Array(2300).join(line);
    var b = new TextEncoder().encode(s);
    var acc = 0;
    for (var i = 0; i < 20; i++)
        acc += new TextDecoder().decode(b).length;
    assert(acc, 20 * s.length, "100KB ascii x20");
}

var tests = [
    test_nonstream_identity,
    test_nonstream_fatal_identity,
    test_ignoreBOM_identity,
    test_stream_carry,
    test_stream_flush,
    test_stream_bom,
    test_stream_fatal,
    test_stream_reset_and_isolation,
    test_stream_views,
    test_strict_decode_options,
    test_strict_ctor_options,
    test_perf_sanity,
];

for (var i = 0; i < tests.length; i++) {
    tests[i]();
    print("ok " + tests[i].name);
}
print("ALL PASS");
