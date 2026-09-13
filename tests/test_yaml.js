/* test_yaml.js -- the YAML 1.2 core-schema subset in dyna:yaml (design 03).
 *
 * The harsh review's verdict on this design was "subset or decline", and what
 * makes a subset honest rather than lossy is that everything outside it is
 * REFUSED BY NAME. So the refusal cases here assert the MESSAGE, not merely
 * that something threw: an anchor must fail because anchors do not exist, not
 * because the line happened to be malformed for some other reason.
 *
 * The Norway problem is a value test, not a syntax test: `no` is the string
 * "no" under YAML 1.2, and a parser that returns false has silently changed a
 * country code into a boolean.
 *
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_yaml.js
 */
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
/* A refusal must name its reason, or it is indistinguishable from a typo. */
function refuses(text, needle, msg) {
    let m = "";
    try { Parse(text); } catch (e) { m = String(e.message); }
    assert(m.indexOf(needle) >= 0, msg + " (message: " + m + ")");
}

/* ---------------------------------------------------------------- scalars */

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

/* THE NORWAY PROBLEM, which is a VALUE bug and not a syntax one. */
same("country: no", { country: "no" }, "`no` is the string no, not false");
same("a: yes\nb: on\nc: off\nd: y\ne: n",
     { a: "yes", b: "on", c: "off", d: "y", e: "n" },
     "yes/on/off/y/n are strings under the 1.2 core schema");
same("version: 1.20", { version: 1.2 }, "1.20 is the number 1.2");
same("version: '1.20'", { version: "1.20" }, "quote it to keep the trailing zero");
same("t: 12:30:00", { t: "12:30:00" }, "sexagesimals are strings, not seconds");

/* --------------------------------------------------------------- mappings */

same("a: 1\nb: 2", { a: 1, b: 2 }, "two entries");
same("a:\n  b:\n    c: deep", { a: { b: { c: "deep" } } }, "nesting by indent");
same("a:\n    b: 1\n    c: 2", { a: { b: 1, c: 2 } }, "any indent width");
same("'a b': 1", { "a b": 1 }, "a quoted key");
same("a: b: c", { a: "b: c" }, "only the FIRST colon splits");
same("url: http://x/y", { url: "http://x/y" }, "a colon with no space is not a split");

/* -------------------------------------------------------------- sequences */

same("- 1\n- 2", [1, 2], "a top-level sequence");
same("a:\n  - x\n  - y", { a: ["x", "y"] }, "a sequence under a key");
same("- - 1", [[1]], "a nested sequence");
same("a:\n  - b: 1\n    c: 2\n  - b: 3\n    c: 4",
     { a: [{ b: 1, c: 2 }, { b: 3, c: 4 }] },
     "the compact `- key: value` form, with continuation lines");
same("- name: x\n  tags:\n    - a\n    - b",
     [{ name: "x", tags: ["a", "b"] }], "and a sequence inside it");
same("a:\n  -\n    b: 1", { a: [{ b: 1 }] }, "a dash on its own line");

/* ------------------------------------------------------------------ flow */

same("a: [1, 2, 3]", { a: [1, 2, 3] }, "a flow sequence");
same("a: {x: 1, y: 2}", { a: { x: 1, y: 2 } }, "a flow mapping");
same("a: [[1, 2], [3]]", { a: [[1, 2], [3]] }, "nested flow");
same("a: [{b: 1}]", { a: [{ b: 1 }] }, "a mapping inside a sequence");
same("a: []\nb: {}", { a: [], b: {} }, "empty flow collections");
same("a: ['x, y', z]", { a: ["x, y", "z"] }, "a comma inside quotes is not a separator");
throws(() => Parse("a: [1, 2"), "an unterminated flow collection is refused");
throws(() => Parse("a: {b}"), "a flow mapping entry needs a colon");

/* --------------------------------------------------------- block scalars */

eq(Parse("s: |\n  one\n  two\n").s, "one\ntwo\n", "a literal block keeps newlines");
eq(Parse("s: >\n  one\n  two\n").s, "one two\n", "a folded block joins them");
eq(Parse("s: |-\n  one\n").s, "one", "the - indicator strips the final newline");
eq(Parse("s: |+\n  one\n\n").s, "one\n\n", "the + indicator keeps them");
eq(Parse("s: |\n  a\n\n  b\n").s, "a\n\nb\n", "a blank line inside a literal block");
eq(Parse("s: |\n  keep: this\n  - and this\n").s, "keep: this\n- and this\n",
   "a block scalar is TEXT: its content is not parsed as YAML");
eq(Parse("a:\n  s: |\n    x\n  t: 1").a.t, 1, "the block ends where the indent does");

/* ----------------------------------------------------- multiple documents */

eq(j(ParseAll("a: 1\n---\nb: 2")), j([{ a: 1 }, { b: 2 }]), "two documents");
eq(j(ParseAll("---\na: 1\n---\nb: 2")), j([{ a: 1 }, { b: 2 }]), "a leading ---");
eq(ParseAll("a: 1").length, 1, "one document is still an array");
eq(ParseAll("").length, 0, "an empty input has no documents");
refuses("a: 1\n---\nb: 2", "ParseAll", "Parse REFUSES a multi-document input, naming ParseAll");

/* --------------------------------- the `...` document-end marker (YAML 1.2) */

/* A `...` line ENDS a document: it must not loop forever appending empty
 * documents, and it must not be refused as "unexpected content". */
eq(j(ParseAll("a: 1\n...\n")), j([{ a: 1 }]),
   "`...` ends the document: exactly one, never a runaway stream of nulls");
eq(j(ParseAll("a: 1\n---\nb: 2\n...\n")), j([{ a: 1 }, { b: 2 }]),
   "two documents closed by a trailing ...");
eq(j(ParseAll("...\n---\na: 1")), j([{ a: 1 }]),
   "an end marker before any document contributes no document");
eq(ParseAll("...\n").length, 0, "a bare end marker is ZERO documents (nothing was open)");
eq(j(Parse("a: 1\n...\n")), j({ a: 1 }), "Parse accepts a trailing ...");
eq(j(Parse("...\n")), j(null),
   "Parse: a bare end marker is an empty stream -- null, no throw");
refuses("a: 1\n...\nb: 2", "ParseAll",
        "content after `...` is still a second document, refused by name");

/* ------------------------------- REFUSED BY NAME, not silently ignored */

/* Each of these has NO other construct that could refuse it: a document with
 * both an anchor and an alias passes the anchor case through the ALIAS guard,
 * and then deleting the anchor check fails nothing. */
refuses("a: &anchor 1", "(&name)", "an anchor ALONE is refused as an anchor");
refuses("b: *anchor", "(*name)", "and an alias alone as an alias");
refuses("a: !!str 1", "tag", "a tag is refused as a tag");
refuses("a: !custom {}", "tag", "including a local tag");
refuses("%YAML 1.2\n---\na: 1", "directive", "a directive is refused as a directive");
refuses("d:\n  <<: x", "merge key", "a merge key is refused as a merge key");
refuses("? complex\n: value", "explicit key", "an explicit key is refused as one");
refuses("a:\n\tb: 1", "tab", "a tab in the indentation is refused as a tab");
{
    /* The whole point of refusing: an anchor SILENTLY dropped would return a
     * document missing a value, and nothing downstream could tell. */
    let got = "parsed";
    try { got = j(Parse("defaults: &d\n  retries: 3\nprod:\n  <<: *d\n")); }
    catch (e) { got = "refused"; }
    eq(got, "refused", "an anchored document fails rather than losing a key");
}

/* ------------------------------------------------------------- refusals */

throws(() => Parse(42), "the input must be a string");
throws(() => Parse(), "and is required");
throws(() => Parse("a: 1\n  b: 2"), "unexpected indentation is refused");
throws(() => Parse("just a scalar\nand another"), "two plain scalars are not a document");
throws(() => Parse('a: "unterminated'), "an unterminated quote");
throws(() => Parse('a: "\\q"'), "an unknown escape");
{
    const deep = "a:\n" + Array.from({ length: 200 }, (_, i) =>
        " ".repeat((i + 1) * 2) + "b:").join("\n") + "\n";
    throws(() => Parse(deep), "nesting past the cap is refused");
}

/* ------------------------------------------- line-table amplification cap */
{
    /* The split stage caps the line table at 4,000,000 lines (YML_MAX_LINES):
     * a refusal here, not a ~96 MB allocation for a junk input. */
    let m = "";
    try { Parse("x\n".repeat(4000001)); } catch (e) { m = String(e.message); }
    assert(/exceeds \d+ lines/.test(m),
           "an over-cap line count is refused by name (got: " + m + ")");
    /* ...while a heavy-but-legal input still parses. */
    eq(ParseAll("# c\n".repeat(1000000)).length, 0,
       "a 1M-line input still parses (the cap is 4,000,000 lines)");
}

/* ------------------------------------------------------------ leading BOM */
eq(Parse("\uFEFFa: 1").a, 1, "a leading UTF-8 BOM is stripped");
eq(j(ParseAll("\uFEFFa: 1\n")), j([{ a: 1 }]), "ParseAll strips it too");
eq(j(Parse("\uFEFF")), j(null), "a BOM alone is an empty stream");

/* ------------------------------------------------- prototype safety */

{
    const o = Parse("__proto__: x\nb: 1");
    eq(o.__proto__, "x", "a __proto__ key is an own property");
    eq(Object.keys(o).length, 2, "and it is enumerable");
    eq(j(Parse("a: {__proto__: 1}")), '{"a":{"__proto__":1}}', "in flow too");
}

/* --------------------------------------------------------- Stringify */

{
    /* THE ORACLE: whatever comes out must parse back to what went in. A round
     * trip is blind to a self-consistent mistake, so the cases below ALSO pin
     * the exact text where the format has a hazard. */
    const cases = [
        { a: 1, b: "two", c: true, d: null },
        { list: [1, 2, 3], nested: { x: { y: "z" } } },
        [{ name: "a" }, { name: "b" }],
        { empty: [], blank: {}, s: "" },
        { tricky: "yes", also: "no", num: "123", colon: "a: b", hash: "a # b" },
        { spaces: "  padded  ", dash: "-x", multi: "line\nbreak" },
        [[1, 2], [3, 4]],
        { deep: { a: [{ b: [1, { c: 2 }] }] } },
    ];
    let bad = 0;
    for (const c of cases) {
        const text = Stringify(c);
        let back;
        try { back = Parse(text); } catch (e) { back = "THREW: " + e.message; }
        if (j(back) !== j(c)) {
            bad++;
            print("  round trip differs for " + j(c));
            print("    emitted: " + JSON.stringify(text));
            print("    back:    " + j(back));
        }
    }
    assert(bad === 0, "every value round-trips through Stringify (" +
                      (cases.length - bad) + "/" + cases.length + ")");
}
eq(Stringify({ a: 1 }), "a: 1\n", "the simplest document");
eq(Stringify({ a: [1, 2] }), "a:\n  - 1\n  - 2\n", "a sequence under a key");
eq(Stringify({ a: { b: 1 } }), "a:\n  b: 1\n", "a nested mapping");
eq(Stringify([1, 2]), "- 1\n- 2\n", "a top-level sequence");
eq(Stringify({ a: "yes" }), 'a: "yes"\n',
   "a 1.1 boolean word is QUOTED on output, for readers that are not 1.2");
eq(Stringify({ a: "123" }), 'a: "123"\n', "and so is a numeric-looking string");
eq(Stringify({ a: 1 }, { indent: 4 }), "a: 1\n", "indent does not affect a flat map");
eq(Stringify({ a: { b: 1 } }, { indent: 4 }), "a:\n    b: 1\n", "but it does a nested one");
eq(Stringify("scalar"), "scalar\n", "a bare scalar document");
eq(Stringify(null), "null\n", "and null");
throws(() => Stringify(), "a value is required");
throws(() => Stringify({}, { indent: 0 }), "indent must be at least 1");

/* ------------------------------------- non-finite numbers round-trip (1.2 core) */
{
    /* NaN/Infinity written bare would RE-PARSE AS STRINGS; the core-schema
     * spellings keep their number-ness across the round trip. */
    const y = Stringify({ a: NaN, b: Infinity, c: -Infinity });
    eq(y, "a: .nan\nb: .inf\nc: -.inf\n", "non-finite numbers emit as .nan/.inf/-.inf");
    const o = Parse(y);
    assert(Number.isNaN(o.a), ".nan re-parses as NaN (a number, not a string)");
    eq(o.b, Infinity, ".inf re-parses as Infinity");
    eq(o.c, -Infinity, "-.inf re-parses as -Infinity");
}
for (const lit of [".inf", ".Inf", "+.inf", "-.inf", "-.Inf", ".nan", ".NaN", ".NAN"]) {
    eq(Parse(Stringify({ a: lit })).a, lit,
       "the literal-lookalike string " + lit + " is quoted and stays a string");
}
eq(Stringify({ a: "..." }), 'a: "..."\n', "a document-marker string is quoted");
eq(Stringify({ a: "---" }), 'a: "---"\n', "and so is the other marker");
eq(Parse(Stringify("...")), "...", "a marker string round-trips as a string");
eq(Parse(Stringify("---")), "---", "both marker strings round-trip");

if (fails) {
    print("test_yaml: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_yaml failed");
}
print("test_yaml: " + n + " assertions, 0 failures");
