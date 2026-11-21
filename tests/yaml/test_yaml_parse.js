// covers: dyna:yaml.Parse
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_yaml_parse");
import { Parse, ParseAll, Stringify } from "dyna:yaml";

let n = 0, fails = 0;
function assert(c, msg) { n++; if (!c) { fails++; print("FAIL: " + msg); } }
function eq(a, b, msg) {
    assert(a === b, msg + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")");
}
const j = (v) => JSON.stringify(v);
function same(text, want, msg) { eq(j(Parse(text)), j(want), msg); }
function throws(fn, msg) {
    let t = false;
    try { fn(); } catch (e) { t = true; }
    assert(t, msg);
}
function refuses(text, needle, msg) {
    let m = "";
    try { Parse(text); } catch (e) { m = String(e.message); }
    assert(m.indexOf(needle) >= 0, msg + " (message: " + m + ")");
}

same("a: 1", { a: 1 }, "an integer");

same("a: -3", { a: -3 }, "a negative integer");

same("a: 0x1F", { a: 31 }, "hex");

same("a: 0o17", { a: 15 }, "octal");

same("a: 1.5", { a: 1.5 }, "a float");

same("a: -2.5e3", { a: -2500 }, "an exponent");

same("a: true\nb: false", { a: true, b: false }, "booleans");

same("a: null\nb: ~\nc:", { a: null, b: null, c: null }, "the three spellings of null");

same("a: hello world", { a: "hello world" }, "a plain string keeps its spaces");

same("a: 'it''s'", { a: "it's" }, "a single-quoted scalar doubles its quote");

same('a: "tab\\there"', { a: "tab\there" }, "a double-quoted escape");

same('a: "\\u00e9"', { a: "é" }, "a \\u escape");

same("a: '123'", { a: "123" }, "a quoted number is a STRING");

same("a: 'true'", { a: "true" }, "and so is a quoted boolean");

same("a: 1 # comment", { a: 1 }, "a trailing comment is not part of the value");

same("a: 'x # y'", { a: "x # y" }, "but a # inside quotes is");

same("# only a comment\na: 1", { a: 1 }, "a whole-line comment");

same("country: no", { country: "no" }, "`no` is the string no, not false");

same("a: yes\nb: on\nc: off\nd: y\ne: n",
     { a: "yes", b: "on", c: "off", d: "y", e: "n" },
     "yes/on/off/y/n are strings under the 1.2 core schema");

same("version: 1.20", { version: 1.2 }, "1.20 is the number 1.2");

same("version: '1.20'", { version: "1.20" }, "quote it to keep the trailing zero");

same("t: 12:30:00", { t: "12:30:00" }, "sexagesimals are strings, not seconds");

same("a: 1\nb: 2", { a: 1, b: 2 }, "two entries");

same("a:\n  b:\n    c: deep", { a: { b: { c: "deep" } } }, "nesting by indent");

same("a:\n    b: 1\n    c: 2", { a: { b: 1, c: 2 } }, "any indent width");

same("'a b': 1", { "a b": 1 }, "a quoted key");

same("a: b: c", { a: "b: c" }, "only the FIRST colon splits");

same("url: http://x/y", { url: "http://x/y" }, "a colon with no space is not a split");

same("- 1\n- 2", [1, 2], "a top-level sequence");

same("a:\n  - x\n  - y", { a: ["x", "y"] }, "a sequence under a key");

same("- - 1", [[1]], "a nested sequence");

same("a:\n  - b: 1\n    c: 2\n  - b: 3\n    c: 4",
     { a: [{ b: 1, c: 2 }, { b: 3, c: 4 }] },
     "the compact `- key: value` form, with continuation lines");

same("- name: x\n  tags:\n    - a\n    - b",
     [{ name: "x", tags: ["a", "b"] }], "and a sequence inside it");

same("a:\n  -\n    b: 1", { a: [{ b: 1 }] }, "a dash on its own line");

same("a: [1, 2, 3]", { a: [1, 2, 3] }, "a flow sequence");

same("a: {x: 1, y: 2}", { a: { x: 1, y: 2 } }, "a flow mapping");

same("a: [[1, 2], [3]]", { a: [[1, 2], [3]] }, "nested flow");

same("a: [{b: 1}]", { a: [{ b: 1 }] }, "a mapping inside a sequence");

same("a: []\nb: {}", { a: [], b: {} }, "empty flow collections");

same("a: ['x, y', z]", { a: ["x, y", "z"] }, "a comma inside quotes is not a separator");

throws(() => Parse("a: [1, 2"), "an unterminated flow collection is refused");

throws(() => Parse("a: {b}"), "a flow mapping entry needs a colon");

eq(Parse("s: |\n  one\n  two\n").s, "one\ntwo\n", "a literal block keeps newlines");

eq(Parse("s: >\n  one\n  two\n").s, "one two\n", "a folded block joins them");

eq(Parse("s: |-\n  one\n").s, "one", "the - indicator strips the final newline");

eq(Parse("s: |+\n  one\n\n").s, "one\n\n", "the + indicator keeps them");

eq(Parse("s: |\n  a\n\n  b\n").s, "a\n\nb\n", "a blank line inside a literal block");

eq(Parse("s: |\n  keep: this\n  - and this\n").s, "keep: this\n- and this\n",
   "a block scalar is TEXT: its content is not parsed as YAML");

eq(Parse("a:\n  s: |\n    x\n  t: 1").a.t, 1, "the block ends where the indent does");

eq(j(Parse("a: 1\n...\n")), j({ a: 1 }), "Parse accepts a trailing ...");

eq(j(Parse("...\n")), j(null),
   "Parse: a bare end marker is an empty stream -- null, no throw");

throws(() => Parse(42), "the input must be a string");

throws(() => Parse(), "and is required");

throws(() => Parse("a: 1\n  b: 2"), "unexpected indentation is refused");

throws(() => Parse("just a scalar\nand another"), "two plain scalars are not a document");

throws(() => Parse('a: "unterminated'), "an unterminated quote");

throws(() => Parse('a: "\\q"'), "an unknown escape");

__kitS.section("nesting past the cap is refused", () => {
    const deep = "a:\n" + Array.from({ length: 200 }, (_, i) =>
        " ".repeat((i + 1) * 2) + "b:").join("\n") + "\n";
    throws(() => Parse(deep), "nesting past the cap is refused");
});

__kitS.section("an over-cap line count is refused by name", () => {
    let m = "";
    try { Parse("x\n".repeat(4000001)); } catch (e) { m = String(e.message); }
    assert(/exceeds \d+ lines/.test(m),
           "an over-cap line count is refused by name (got: " + m + ")");
    eq(ParseAll("# c\n".repeat(1000000)).length, 0,
       "a 1M-line input still parses (the cap is 4,000,000 lines)");
});

eq(Parse("\uFEFFa: 1").a, 1, "a leading UTF-8 BOM is stripped");

eq(j(Parse("\uFEFF")), j(null), "a BOM alone is an empty stream");

__kitS.section("__proto__: x b: 1", () => {
    const o = Parse("__proto__: x\nb: 1");
    eq(o.__proto__, "x", "a __proto__ key is an own property");
    eq(Object.keys(o).length, 2, "and it is enumerable");
    eq(j(Parse("a: {__proto__: 1}")), '{"a":{"__proto__":1}}', "in flow too");
});

for (const lit of [".inf", ".Inf", "+.inf", "-.inf", "-.Inf", ".nan", ".NaN", ".NAN"]) {
    eq(Parse(Stringify({ a: lit })).a, lit,
       "the literal-lookalike string " + lit + " is quoted and stays a string");
}

eq(Parse(Stringify("...")), "...", "a marker string round-trips as a string");

eq(Parse(Stringify("---")), "---", "both marker strings round-trip");

same("--- 5", 5, "a document node may start on the --- line");

same("--- {a: 1}", { a: 1 }, "a flow node on the marker line");

same("--- 'x'", "x", "a quoted node on the marker line");

same("---a: 1", { "---a": 1 },
     "`---` needs a blank after it: `---a` is a plain key, not a marker");

same("---x", "---x", "`---x` is the scalar it spells");

refuses("a: 1\n...x", "mapping entry",
        "`...x` is content, silently swallowing it loses bytes");
refuses("a: 1\n... junk", "document-end marker",
        "content after `...` on its line is refused, not dropped");
same("---: v", { "---": "v" }, "`---: v` is a mapping whose key is ---");

for (const lit of [".inf", ".Inf", ".INF", "+.inf", "+.Inf", "+.INF"]) {
    eq(Parse("k: " + lit).k, Infinity, lit + " is Infinity (1.2 core float rule)");
}

for (const lit of ["-.inf", "-.Inf", "-.INF"]) {
    eq(Parse("k: " + lit).k, -Infinity, lit + " is -Infinity");
}

eq(Parse('a: "\\x41"').a, "A", "\\xHH is a code point escape");

eq(Parse('a: "\\U0001F600"').a, "😀", "\\UHHHHHHHH reaches past the BMP");

eq(Parse('a: "\\a\\v\\e"').a, "\x07\x0b\x1b", "\\a \\v \\e are the control spells");

eq(Parse('a: "\\N\\_\\L\\P"').a, "\u0085\u00A0\u2028\u2029",
   "\\N \\_ \\L \\P are the line/spacing spells");

eq(Parse('a: "a\\ b"').a, "a b", "\\ escapes a space");

throws(() => Parse('a: "\\xZZ"'), "a bad \\x is refused");

throws(() => Parse('a: "\\U00110000"'), "past U+10FFFF is refused");

same('"a\\"b": 1', { 'a"b': 1 },
     "a \\\" escape inside a quoted key does not hide the colon");

same('"a":1', { a: 1 }, "a quoted key may touch its colon");

same('{"a:b": 1}', { "a:b": 1 }, "a flow-mapping key may contain a colon");

refuses('"a" junk: 1', "quoted key", "junk between a quoted key and its colon");
refuses('{ "a" junk: 1 }', "quoted key", "in flow too");
refuses('a: "x" junk', "after the value",
        "trailing junk after a quoted scalar is refused, never dropped");
refuses("a: 'x' junk", "after the value", "single-quoted too");
refuses("a: [1] junk", "after the value", "and after a flow sequence");
refuses("a: {a: 1} junk", "after the value", "and after a flow mapping");
same("a: 'x' # c", { a: "x" }, "but a comment after the value is fine");

same("a: [1] # ok", { a: [1] }, "also after a flow sequence");

same("[a: 1]", [{ a: 1 }],
     "a flow-sequence entry with `k: v` is a single-pair mapping (1.2)");

same("[a: 1, b: 2]", [{ a: 1 }, { b: 2 }], "each entry pairs on its own");

same("['k': 1]", [{ k: 1 }], "a quoted key pairs too");

same("[http://x]", ["http://x"],
     "a colon with no space stays INSIDE a plain scalar: URLs survive");

same("[a:1]", ["a:1"], "and `a:1` without a space is still a scalar");

same("[[a: 1]]", [[{ a: 1 }]], "nested single pairs");

same("a:\tb", { a: "b" },
     "a tab after the colon separates, like a space (only INDENT tabs die)");

same("a: \tb", { a: "b" }, "a tab before the value separates too");

same("[1,\t2]", [1, 2], "a tab is a flow separator");

same("{a:\t1}", { a: 1 }, "in flow mappings as well");

same("s: |\n  a\n\t\n  b\n", { s: "a\n\nb\n" },
     "a tab-only line is a blank line inside a block scalar");

refuses("a:\n  \tb: 1", "tab",
        "a tab INDENTING content is still refused (spaces only indent)");
refuses("s: |\n  a\n\tb\n", "tab",
        "and a tab-indented block-scalar body line with it");

throws(() => Parse("a: one\n  two\n"),
       "a multi-line plain scalar is OUT (refused, not folded)");

throws(() => Parse("a: [1,\n2]"),
       "a multi-line flow collection is OUT (single-line only)");

same('a: "one\n  two"', { a: "one two" },
     "a multi-line quoted scalar folds to a space (YAML 1.2 flow scalars)");

same("a: 1\rb: 2", { a: "1\rb: 2" },
     "a lone CR is not a line break here (\\n and \\r\\n are)");

same("[a,b]: v", { "[a,b]": "v" },
     "a collection key is taken as the literal string (complex keys are OUT)");

assert(Parse("k: 9223372036854775808", { bigint: true }).k === 9223372036854775808n,
   "an int past 64 bits is an exact BigInt under { bigint: true }: never a wrap and never a lossy double");
assert(Parse("k: 9223372036854775807").k === 9223372036854775807,
   "by default (D8) an int past 2^53 is the rounded Number, like JSON.parse");

same(": v", { "": "v" }, "an empty key is the empty string, not null");

same("a: 12:30:00", { a: "12:30:00" }, "sexagesimal-looking values stay strings");

same("k: 0b101", { k: "0b101" }, "0b is not a 1.2 core radix: string");

__kitS.section("2: a 1: b", () => {
    const o = Parse("2: a\n1: b");
    eq(Object.keys(o).join(), "1,2",
       "integer-like keys enumerate in numeric order (JS object semantics)");
});
__kitS.end();

eq(Parse("s: >\n  a\n   b\n  c\n").s, "a  b c\n",
   "PINNED DIVERGENCE: `>` folds a more-indented line flat (spec keeps its breaks)");

if (fails) {
    print("test_yaml: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_yaml failed");
}
print("test_yaml: " + n + " assertions, 0 failures");
