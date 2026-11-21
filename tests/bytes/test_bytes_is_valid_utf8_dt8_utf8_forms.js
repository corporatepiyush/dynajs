// covers: dyna:bytes.isValidUtf8
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_bytes_is_valid_utf8_dt8_utf8_forms");
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

__kitS.section(".isValidUtf8", () => {
for (const [str, want] of SHAPES) {
    const t = new Text(str);
    ok(t.isValidUtf8 === want,
       "Text(" + JSON.stringify(str) + ").isValidUtf8 === " + want, t.isValidUtf8);
    ok(isValidUtf8(str) === want,
       "free isValidUtf8(string " + JSON.stringify(str) + ") === " + want,
       isValidUtf8(str));
    ok(t.isValidUtf8 === isValidUtf8(str),
       "Text getter and free function agree on " + JSON.stringify(str));
}
});
__kitS.end();

if (fails) {
    print("test_dt8_utf8_forms: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_dt8_utf8_forms failed");
}
print("test_dt8_utf8_forms: " + n + " assertions, 0 failures");
