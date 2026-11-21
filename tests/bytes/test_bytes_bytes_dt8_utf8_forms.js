// covers: dyna:bytes.Bytes
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_bytes_bytes_dt8_utf8_forms");
import { Text, Bytes, isValidUtf8 } from "dyna:bytes";

// the raw (WTF-8) encoding keeps lone surrogates, so invalid strings yield invalid bytes
const fromUtf8 = (s) => new Text(s).toBytes().array;

let n = 0, fails = 0;
function ok(c, m, extra) {
    n++;
    if (!c) { fails++; print("FAIL: " + m + (extra !== undefined ? " (" + extra + ")" : "")); }
}

const SHAPES = [
    ["", true],
    ["abc", true],
    ["a\u0000\u001F\u007F", true],
    ["héllo", true],
    ["日本語", true],
    ["a→b", true],
    ["😀", true],
    ["👍🏽", true],
    ["\u{10FFFF}", true],
    ["e\u0301", true],
    ["\uD800", false],
    ["\uDC00", false],
    ["\uD800x", false],
    ["x\uDC00", false],
    ["\uD800\uDC00", true],
    ["\uD800\uD800", false],
    ["\uDC00\uDC00", false],
    ["😀\uD800", false],
];

__kitS.section("Bytes getter and free function agree over the encoding of", () => {
for (const [str] of SHAPES) {
    const bytes = fromUtf8(str);
    const b = new Bytes(bytes);
    ok(b.isValidUtf8 === isValidUtf8(bytes),
       "Bytes getter and free function agree over the encoding of " +
       JSON.stringify(str));
    if (b.isValidUtf8 === false)
        ok(isValidUtf8(str) === false,
           "bytes-level false implies string-level false for " + JSON.stringify(str));
}
});

__kitS.section("well-formed", () => {
for (const [str, want] of SHAPES) {
    if (want)
        ok(new Bytes(fromUtf8(str)).isValidUtf8 === true,
           "well-formed " + JSON.stringify(str) + " encodes to valid UTF-8 bytes");
}
});
__kitS.end();

if (fails) {
    print("test_dt8_utf8_forms: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_dt8_utf8_forms failed");
}
print("test_dt8_utf8_forms: " + n + " assertions, 0 failures");
