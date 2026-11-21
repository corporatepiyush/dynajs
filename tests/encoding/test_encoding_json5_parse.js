// covers: dyna:encoding.JSON5Parse
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_encoding_json5_parse");
import { JSON5Parse, JSON5Stringify, StableStringify } from "dyna:encoding";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function eq(a, b, msg) {
    assert(a === b, msg + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
}
function throws(fn, msg) {
    let t = false;
    try { fn(); } catch (e) { t = true; }
    assert(t, msg);
}

for (const src of ['{"a":1}', '[1,2,3]', '"str"', '123', '-1.5e10', 'true',
                   'false', 'null', '{}', '[]', '{"a":{"b":[1,{"c":2}]}}']) {
    eq(JSON.stringify(JSON5Parse(src)), JSON.stringify(JSON.parse(src)),
       "JSON5 parses valid JSON identically: " + src);
}

eq(JSON5Parse("{a:1}").a, 1, "unquoted key");

eq(JSON5Parse("{$a:1,_b:2}").$a, 1, "unquoted key with $");

eq(JSON5Parse("{_b:2}")._b, 2, "unquoted key with _");

eq(JSON5Parse("{'a':1}").a, 1, "single-quoted key");

eq(JSON5Parse("'hi'"), "hi", "single-quoted string");

eq(JSON5Parse('{a:1,}').a, 1, "trailing comma in an object");

eq(JSON5Parse("[1,2,]").length, 2, "trailing comma in an array");

eq(JSON5Parse("// c\n1"), 1, "line comment");

eq(JSON5Parse("/* c */ 1"), 1, "block comment");

eq(JSON5Parse("{a:1 /* mid */, b:2}").b, 2, "comment between members");

eq(JSON5Parse("0x10"), 16, "hex integer");

eq(JSON5Parse("0xFF"), 255, "uppercase hex");

eq(JSON5Parse("-0x10"), -16, "signed hex");

eq(JSON5Parse("0x1FFFFFFFFFFFFF"), 9007199254740991, "hex at 2^53-1 is exact");

eq(JSON5Parse("0x20000000000000"), 9007199254740992, "hex at 2^53 exactly is allowed");

eq(JSON5Parse("0x20000000000001"), 9007199254740992,
   "hex past 2^53 rounds to the nearest double, not wrapped");

eq(JSON5Parse("0xFFFFFFFFFFFFFFFF"), Math.pow(2, 64), "a 16-F literal rounds to 2^64");

eq(JSON5Parse("0x" + "F".repeat(40)), Math.pow(2, 160), "and a 40-digit literal rounds to 2^160");

__kitS.section("a 100-digit number parses and stays finite", () => {
    const digits = "1234567890".repeat(10);
    const v = JSON5Parse(digits);
    assert(Number.isFinite(v), "a 100-digit number parses and stays finite");
    eq(v, Number(digits), "and equals Number() of the same digits");
    const frac = JSON5Parse(digits + "." + digits);
    assert(Number.isFinite(frac), "a 200-character fraction parses too");
    const neg = JSON5Parse("-" + digits + "e-10");
    assert(Number.isFinite(neg) && neg < 0, "signed, exponentiated, still finite");
});

eq(JSON5Parse("+5"), 5, "leading plus");

eq(JSON5Parse(".5"), 0.5, "leading decimal point");

eq(JSON5Parse("5."), 5, "trailing decimal point");

eq(JSON5Parse("Infinity"), Infinity, "Infinity");

eq(JSON5Parse("-Infinity"), -Infinity, "-Infinity");

assert(Number.isNaN(JSON5Parse("NaN")), "NaN");

eq(JSON5Parse('"a\\\nb"'), "ab", "line continuation in a string");

eq(JSON5Parse('"\\x41"'), "A", "\\x escape");

eq(JSON5Parse('"\\u0041"'), "A", "\\u escape");

eq(JSON5Parse('"\\u{0041}"'.replace("{0041}", "0041")), "A", "\\u escape again");

eq(JSON5Parse('"\\uD83D\\uDE00"'), "\u{1f600}", "surrogate pair recombines");

eq(JSON5Parse('"\\a"'), "a", "unknown escape is the literal character");

eq(JSON5Parse("  \n\t {  a : 1 }  ").a, 1, "whitespace everywhere");

eq(JSON5Parse("[[[[1]]]]")[0][0][0][0], 1, "nested arrays");

throws(() => JSON5Parse("{a:1"), "unterminated object throws");

throws(() => JSON5Parse("[1,2"), "unterminated array throws");

throws(() => JSON5Parse('"abc'), "unterminated string throws");

throws(() => JSON5Parse("/* unterminated"), "unterminated block comment throws");

throws(() => JSON5Parse("{a:1} trailing"), "trailing content throws");

throws(() => JSON5Parse(""), "empty input throws");

throws(() => JSON5Parse("{,}"), "a bare comma is not a key");

throws(() => JSON5Parse('"a\nb"'), "a raw newline in a string throws");

throws(() => JSON5Parse('"\\xZZ"'), "a bad hex escape throws");

throws(() => JSON5Parse("0x"), "0x with no digits throws");

for (const bad of [null, undefined, 42, {}])
    throws(() => JSON5Parse(bad), "JSON5Parse refuses " + JSON.stringify(bad));

throws(() => JSON5Parse("[".repeat(5000) + "]".repeat(5000)),
       "5000-deep nesting is refused, not crashed into");

eq(JSON5Parse("[".repeat(200) + "]".repeat(200)).length, 1,
   "200-deep nesting still parses");

__kitS.section("{\"__proto__\": {\"polluted\": true}}", () => {
    const o = JSON5Parse('{"__proto__": {"polluted": true}}');
    assert(Object.prototype.hasOwnProperty.call(o, "__proto__"),
           "JSON5 __proto__ is an OWN property");
    eq(({}).polluted, undefined, "JSON5 did not pollute Object.prototype");
});
__kitS.end();

for (const v of [{ a: 1, b: [1, 2, { c: "x" }] }, [], {}, "s", 1.5, -0.25]) {
    eq(JSON.stringify(JSON5Parse(JSON5Stringify(v))), JSON.stringify(v),
       "JSON5 round trip: " + JSON.stringify(v));
}

throws(() => JSON5Parse("'\\1'"), "\\1 is refused");

throws(() => JSON5Parse("'\\9'"), "\\9 is refused");

throws(() => JSON5Parse("'\\01'"), "\\0 followed by a digit is refused");

eq(JSON5Parse("'\\0'"), "\0", "lone \\0 still decodes to NUL");

eq(JSON5Parse("\u00a01"), 1, "NBSP is whitespace");

eq(JSON5Parse("\ufeff1"), 1, "ZWNBSP is whitespace");

eq(JSON5Parse("\u20031"), 1, "U+2003 (Zs) is whitespace");

eq(JSON5Parse("\u202f1"), 1, "U+202F is whitespace");

eq(JSON5Parse("\u30001"), 1, "U+3000 is whitespace");

eq(JSON5Parse("1\u00a0"), 1, "trailing NBSP is whitespace");

if (fails) {
    print("test_json5: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_json5 failed");
}
print("test_json5: " + n + " assertions, 0 failures");
