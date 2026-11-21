// covers: dyna:bytes.Text
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_bytes_text_dt8_utf8_forms");
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

__kitS.section("Text.prototype.isValidUtf8 is a getter with no setter", () => {
    const proto = Object.getPrototypeOf(new Text(""));
    const d = Object.getOwnPropertyDescriptor(proto, "isValidUtf8");
    ok(!!d && typeof d.get === "function" && d.set === undefined,
       "Text.prototype.isValidUtf8 is a getter with no setter");
    const bd = Object.getOwnPropertyDescriptor(
        Object.getPrototypeOf(new Bytes(new Uint8Array(0))), "isValidUtf8");
    ok(!!bd && typeof bd.get === "function" && bd.set === undefined,
       "Bytes.prototype.isValidUtf8 is a getter with no setter (unchanged)");
    ok(typeof isValidUtf8 === "function", "the free function is still a function");

    let threw = false;
    try { new Text("x").isValidUtf8(); } catch { threw = true; }
    ok(threw, "the historical method spelling is no longer callable " +
              "(one property per name; bytes.isValidUtf8() never was either)");
});

__kitS.section("a string cut through a surrogate pair is ill-formed (first half)", () => {
    const s = "a😀b";
    const half1 = s.slice(0, 2);
    const half2 = s.slice(2);
    ok(new Text(half1).isValidUtf8 === false && isValidUtf8(half1) === false,
       "a string cut through a surrogate pair is ill-formed (first half)");
    ok(new Text(half2).isValidUtf8 === false && isValidUtf8(half2) === false,
       "a string cut through a surrogate pair is ill-formed (second half)");
    ok(new Text(s).isValidUtf8 === true, "the whole string is well-formed");
});
__kitS.end();

if (fails) {
    print("test_dt8_utf8_forms: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_dt8_utf8_forms failed");
}
print("test_dt8_utf8_forms: " + n + " assertions, 0 failures");
