/* test_dt8_utf8_forms.js --: isValidUtf8 has one predicate and three
 * spellings, and they must agree on every Unicode shape.
 *
 *   text.isValidUtf8 GETTER on dyna:bytes' Text (was a method;
 *                                                        converted the member
 *                                                        to an accessor so it
 *                                                        reads like Bytes')
 *   bytes.isValidUtf8    GETTER on Bytes
 *   isValidUtf8(x)       free function over a string or a byte view
 *
 * A JS string is ill-formed UTF-8 only through a LONE SURROGATE (a paired
 * surrogate encodes to valid UTF-8); the byte-level spellings additionally
 * see raw bytes through Bytes/Uint8Array. The table below pins, per shape,
 * what the string-level predicate answers, and that Text, Bytes and the free
 * function never disagree where they observe the same bytes.
 *
 * Run: dynajs (CONFIG_NATIVE_MODULES=y) tests/test_dt8_utf8_forms.js
 */
import { Text, Bytes, isValidUtf8, fromUtf8 } from "dyna:bytes";

let n = 0, fails = 0;
function ok(c, m, extra) {
    n++;
    if (!c) { fails++; print("FAIL: " + m + (extra !== undefined ? " (" + extra + ")" : "")); }
}

/* [string, string-level validity] -- the pinned truth table. Lone surrogates
 * are the ONLY way a JS string encodes to ill-formed UTF-8. */
const SHAPES = [
    ["", true],                            // empty
    ["abc", true],                         // ASCII
    ["a\u0000\u001F\u007F", true],         // control bytes are well-formed
    ["héllo", true],                       // Latin-1 supplement (2-byte)
    ["日本語", true],                       // BMP, 3-byte UTF-8
    ["a→b", true],                         // arrows
    ["😀", true],                          // astral via a PAIRED surrogate
    ["👍🏽", true],                          // astral + skin-tone modifier
    ["\u{10FFFF}", true],                  // max code point (4-byte)
    ["e\u0301", true],                     // combining accent
    ["\uD800", false],                     // lone HIGH surrogate
    ["\uDC00", false],                     // lone LOW surrogate
    ["\uD800x", false],                    // lone high, then text
    ["x\uDC00", false],                    // text, then lone low
    ["\uD800\uDC00", true],                // a PROPER pair (encodes 😀)
    ["\uD800\uD800", false],               // two highs: the second is lone
    ["\uDC00\uDC00", false],               // two lows
    ["😀\uD800", false],                   // valid pair then a lone high
];

/* ---- 1. Text getter vs the free function over the SAME STRING ----------- */
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

/* ---- 2. the byte-level forms agree with each other ---------------------- */
for (const [str] of SHAPES) {
    const bytes = fromUtf8(str);          // the string's actual encoding
    const b = new Bytes(bytes);
    ok(b.isValidUtf8 === isValidUtf8(bytes),
       "Bytes getter and free function agree over the encoding of " +
       JSON.stringify(str));
    /* fromUtf8 of a WELL-FORMED string is valid UTF-8; a lone surrogate was
     * encoded to WTF-8-style bytes by the string->bytes boundary, which the
     * byte-level predicate is NOT required to call invalid (pre-existing
     * contract of the byte view). What IS pinned: the BYTE spellings never
     * disagree with each other, and well-formed inputs answer true. */
    if (b.isValidUtf8 === false)
        ok(isValidUtf8(str) === false,
           "bytes-level false implies string-level false for " + JSON.stringify(str));
}
for (const [str, want] of SHAPES) {
    if (want)
        ok(new Bytes(fromUtf8(str)).isValidUtf8 === true,
           "well-formed " + JSON.stringify(str) + " encodes to valid UTF-8 bytes");
}

/* ---- 3. the member is an accessor, not a method ------------------------- */
{
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
}

/* ---- 4. slices/pieces: a Text whose string contains an astral char, then
 * split at the surrogate-pair boundary -- the string-level answers must stay
 * consistent with the free function per piece. */
{
    const s = "a😀b";
    const half1 = s.slice(0, 2);          // "a" + high surrogate
    const half2 = s.slice(2);             // low surrogate + "b"
    ok(new Text(half1).isValidUtf8 === false && isValidUtf8(half1) === false,
       "a string cut through a surrogate pair is ill-formed (first half)");
    ok(new Text(half2).isValidUtf8 === false && isValidUtf8(half2) === false,
       "a string cut through a surrogate pair is ill-formed (second half)");
    ok(new Text(s).isValidUtf8 === true, "the whole string is well-formed");
}

if (fails) {
    print("test_dt8_utf8_forms: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_dt8_utf8_forms failed");
}
print("test_dt8_utf8_forms: " + n + " assertions, 0 failures");
