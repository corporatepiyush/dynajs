// flags: --std
// covers: integration c:src/core/dyn-textdec.c c:src/core/dyn-textdec-index.h -- legacy decoders use the generated index tables
// timeout: 120
// Exhaustive oracle for the legacy multi-byte TextDecoder tables. The WPT legacy-mb-* decode
// suites are HTML pages (iframe + DOM) that tools/wpt.js skips as needs-dom, so this test reads
// their fixtures directly: every *_chars.html span carries data-cp (the code point the WPT
// generator encoded) and holds those bytes in the page's own encoding; *_errors.html spans are
// checked against the assert_equals strings of the matching *-decode-errors.html page. Each
// fixture page is decoded as ONE stream (iso-2022-jp state carries across spans, as in a
// browser), whole and again in 7-byte stream:true slices. The jis encoders map U+00A5, U+203E and
// U+2212 one way (to 0x5C, 0x7E and U+FF0D's bytes), so those spans expect the round-tripped value
// (iso-2022-jp keeps U+00A5/U+203E through its Roman escape). gb18030/gbk have no such fixtures;
// their table is covered by the WPT gb18030-decoder/gbk-decoder any.js tests in the WPT run.
// Skips (exit 0, SKIPPED) without the third_party/wpt checkout (WPT_DIR overrides).
import { suite } from "../lib/kit.js";
import { readFile, Path } from "dyna:file";

const WPT = std.getenv("WPT_DIR") || "/Users/piyush/ai/dynascript/third_party/wpt";
const ROOT = WPT + "/encoding";
const t = suite("test_wpt_legacy_decode");

function bytes(path) {
    try { return readFile(new Path(path), { bytes: true }); } catch { return null; }
}

function decodeAll(enc, b, step) {
    if (!step) return new TextDecoder(enc).decode(b);
    const d = new TextDecoder(enc);
    let s = "";
    for (let i = 0; i < b.length; i += step) s += d.decode(b.subarray(i, i + step), { stream: true });
    return s + d.decode();
}

const ONE_WAY = { 0xA5: 0x5C, 0x203E: 0x7E, 0x2212: 0xFF0D };

function spans(text) {
    return [...text.matchAll(/<span(?: data-cp="([0-9A-Fa-f]+)" data-bytes="([^"]*)")?>([^<]*)<\/span>/g)];
}

const SETS = [
    ["euc-jp", "legacy-mb-japanese/euc-jp", "eucjp"],
    ["iso-2022-jp", "legacy-mb-japanese/iso-2022-jp", "iso2022jp"],
    ["shift_jis", "legacy-mb-japanese/shift_jis", "sjis"],
    ["euc-kr", "legacy-mb-korean/euc-kr", "euckr"],
    ["big5", "legacy-mb-tchinese/big5", "big5"],
];

if (bytes(ROOT + "/resources/encodings.js") === null) {
    print("SKIPPED: no WPT checkout at " + WPT);
} else {
    for (const [enc, dir, stem] of SETS) {
        t.test(enc + " chars: every encoded pointer decodes to its code point", ({ eq, ok }) => {
            const files = [stem + "_chars.html"].concat(enc === "big5" ? ["big5_chars_extra.html"] : []);
            for (const f of files) {
                const b = bytes(ROOT + "/" + dir + "/" + f);
                ok(b !== null, f + " present");
                for (const step of [0, 7]) {
                    const all = spans(decodeAll(enc, b, step)).filter((m) => m[1]);
                    ok(all.length > 100, f + " has spans (" + all.length + ")");
                    let bad = 0, first = "";
                    for (const m of all) {
                        const cp = parseInt(m[1], 16), want = String.fromCodePoint((enc === "iso-2022-jp" && cp !== 0x2212 ? undefined : ONE_WAY[cp]) ?? cp);
                        if (m[3] !== want && !bad++) first = m[2] + " -> " + JSON.stringify(m[3]) + " want U+" + m[1];
                    }
                    eq(bad, 0, f + (step ? " sliced" : "") + " mismatches" + (first ? " (first: " + first + ")" : ""));
                }
            }
        });
        t.test(enc + " errors: replacement placement matches the WPT page", ({ eq, ok }) => {
            const page = std.loadFile(ROOT + "/" + dir + "/" + stem + "-decode-errors.html");
            ok(page !== null, "errors page present");
            const want = [...page.matchAll(/assert_equals\(\s*nodes\[[^\]]+\]\.textContent,\s*"((?:[^"\\]|\\.)*)"/g)].map((m) => JSON.parse('"' + m[1] + '"'));
            const b = bytes(ROOT + "/" + dir + "/" + stem + "_errors.html");
            for (const step of [0, 7]) {
                const got = spans(decodeAll(enc, b, step)).map((m) => m[3]);
                ok(want.length > 5, "expectations parsed (" + want.length + ")");
                eq(JSON.stringify(got.slice(0, want.length)), JSON.stringify(want), enc + (step ? " sliced" : "") + " error spans");
            }
        });
    }
}

await t.run();
