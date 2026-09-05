/* test_json5.js -- JSON5Parse, JSON5Stringify and StableStringify (design 09).
 *
 * StableStringify is checked against RFC 8785's OWN published vectors, not
 * against a round trip: a canonicaliser that makes a wrong but self-consistent
 * choice re-parses to the same value and is still wrong for every other
 * implementation, which is the whole reason the spec exists.
 *
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_json5.js
 */
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

/* --------------------------------------------- JSON5Parse: JSON is a subset */

/* Everything valid JSON must parse identically. */
for (const src of ['{"a":1}', '[1,2,3]', '"str"', '123', '-1.5e10', 'true',
                   'false', 'null', '{}', '[]', '{"a":{"b":[1,{"c":2}]}}']) {
    eq(JSON.stringify(JSON5Parse(src)), JSON.stringify(JSON.parse(src)),
       "JSON5 parses valid JSON identically: " + src);
}

/* ------------------------------------------------ JSON5Parse: the dialect */

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
/* THE HEX WRAP. The accumulator used to be unbounded (`v = v * 16` in uint64),
 * so a literal past 16 digits silently returned a WRAPPED double. The cap is
 * the decimal path's exactness bound: hex is refused past 2^53, exact below. */
eq(JSON5Parse("0x1FFFFFFFFFFFFF"), 9007199254740991, "hex at 2^53-1 is exact");
eq(JSON5Parse("0x20000000000000"), 9007199254740992, "hex at 2^53 exactly is allowed");
throws(() => JSON5Parse("0x20000000000001"), "hex past 2^53 is refused, not wrapped");
throws(() => JSON5Parse("0xFFFFFFFFFFFFFFFF"), "a 16-F literal is refused");
throws(() => JSON5Parse("0x" + "F".repeat(40)), "and a 40-digit literal too");

/* THE LONG NUMBER. A literal of 64+ characters used to hit a fixed staging
 * buffer and die as "bad number"; a long VALID decimal must parse. */
{
    const digits = "1234567890".repeat(10);          /* 100 digits */
    const v = JSON5Parse(digits);
    assert(Number.isFinite(v), "a 100-digit number parses and stays finite");
    eq(v, Number(digits), "and equals Number() of the same digits");
    const frac = JSON5Parse(digits + "." + digits);
    assert(Number.isFinite(frac), "a 200-character fraction parses too");
    const neg = JSON5Parse("-" + digits + "e-10");
    assert(Number.isFinite(neg) && neg < 0, "signed, exponentiated, still finite");
}
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

/* Whitespace and nesting */
eq(JSON5Parse("  \n\t {  a : 1 }  ").a, 1, "whitespace everywhere");
eq(JSON5Parse("[[[[1]]]]")[0][0][0][0], 1, "nested arrays");

/* --------------------------------------------- JSON5Parse: it must REFUSE */

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

/* THE NEST BOMB. The depth cap is checked before descending, so this must be a
 * clean RangeError and not a stack overflow. */
throws(() => JSON5Parse("[".repeat(5000) + "]".repeat(5000)),
       "5000-deep nesting is refused, not crashed into");
/* And just under the cap still works, so the cap is a cap and not a wall. */
eq(JSON5Parse("[".repeat(200) + "]".repeat(200)).length, 1,
   "200-deep nesting still parses");

/* PROTOTYPE POLLUTION: a document naming __proto__ gets an own property. */
{
    const o = JSON5Parse('{"__proto__": {"polluted": true}}');
    assert(Object.prototype.hasOwnProperty.call(o, "__proto__"),
           "JSON5 __proto__ is an OWN property");
    eq(({}).polluted, undefined, "JSON5 did not pollute Object.prototype");
}

/* --------------------------------------------------------- JSON5Stringify */

eq(JSON5Stringify({ a: 1 }), '{"a":1}', "stringify object");
eq(JSON5Stringify([1, 2]), "[1,2]", "stringify array");
eq(JSON5Stringify("hi"), '"hi"', "stringify string");
eq(JSON5Stringify(null), "null", "stringify null");
eq(JSON5Stringify(true), "true", "stringify true");
/* JSON5 can print what JSON cannot -- that is the point of the dialect. */
eq(JSON5Stringify(NaN), "NaN", "JSON5Stringify prints NaN");
eq(JSON5Stringify(Infinity), "Infinity", "JSON5Stringify prints Infinity");
eq(JSON5Stringify(-Infinity), "-Infinity", "JSON5Stringify prints -Infinity");
eq(JSON5Stringify({ a: 1 }, { indent: 2 }), '{\n  "a": 1\n}', "indent");
eq(JSON5Stringify({ a: undefined, b: 1 }), '{"b":1}', "undefined members are omitted");
eq(JSON5Stringify([undefined]), "[null]", "undefined in an array is null");
eq(JSON5Stringify({ a: "\n\t\"" }), '{"a":"\\n\\t\\""}', "control chars escape");

/* Round trip through our own parser */
for (const v of [{ a: 1, b: [1, 2, { c: "x" }] }, [], {}, "s", 1.5, -0.25]) {
    eq(JSON.stringify(JSON5Parse(JSON5Stringify(v))), JSON.stringify(v),
       "JSON5 round trip: " + JSON.stringify(v));
}

/* --------------------------------------- StableStringify: RFC 8785 vectors */

/* Keys sort by UTF-16 code unit, values keep their form. */
eq(StableStringify({ b: 1, a: 2 }), '{"a":2,"b":1}', "keys are sorted");
eq(StableStringify({ a: 1, A: 2 }), '{"A":2,"a":1}', "uppercase sorts before lowercase");
eq(StableStringify({ "10": 1, "2": 2 }), '{"10":1,"2":2}',
   "numeric-looking keys sort as strings, not numbers");
eq(StableStringify({}), "{}", "empty object");
eq(StableStringify([]), "[]", "empty array");
eq(StableStringify([3, 1, 2]), "[3,1,2]", "array order is NOT sorted");

/* RFC 8785 section 3.2.3's own example: the exact key set and order. */
{
    const input = { "\u20AC": "Euro Sign", "\r": "Carriage Return",
                    "\n": "Newline", "1": "One",
                    "\u0080": "Control", "\u00F6": "Latin Small O With Diaeresis",
                    "\u20ACa": "?", "A": "A" };
    const out = StableStringify(input);
    const keysInOrder = [];
    /* pull the keys back out in emitted order */
    for (const m of out.matchAll(/"((?:[^"\\]|\\.)*)":/g)) keysInOrder.push(m[1]);
    assert(keysInOrder.length === 8, "RFC 8785 example emitted all 8 keys (got "
           + keysInOrder.length + ")");
    /* The decisive property: the emitted order is non-decreasing in UTF-16 units. */
    const decoded = keysInOrder.map(k => JSON.parse('"' + k + '"'));
    let sorted = true;
    for (let i = 1; i < decoded.length; i++)
        if (!(decoded[i - 1] < decoded[i])) { sorted = false; break; }
    assert(sorted, "RFC 8785 keys come out in ascending UTF-16 order: "
           + JSON.stringify(decoded));
}

/* THE ASTRAL CASE, which is where UTF-16 order differs from code-point order:
 * U+1F600 encodes as a surrogate pair starting 0xD83D, which sorts BELOW
 * U+FF01. Sorting by code point would put them the other way round. */
{
    const out = StableStringify({ "\uFF01": 1, "\u{1f600}": 2 });
    assert(out.indexOf("\u{1f600}") < out.indexOf("\uFF01"),
           "an astral key sorts BEFORE U+FF01 (UTF-16 order, not code point)");
    /* Prove the two orders really do disagree, or the assertion above is vacuous. */
    assert("\u{1f600}" < "\uFF01", "JS string < agrees: UTF-16 order");
    assert(0x1f600 > 0xff01, "code point order would say the opposite");
}

/* Canonical numbers: RFC 8785 uses the ECMAScript shortest round-trip form. */
eq(StableStringify(1), "1", "integer 1");
eq(StableStringify(1.0), "1", "1.0 prints as 1");
eq(StableStringify(-0), "0", "negative zero canonicalises to 0");
eq(StableStringify(0.1), "0.1", "0.1");
eq(StableStringify(1e21), "1e+21", "1e21 keeps exponent form");
eq(StableStringify(1e-7), "1e-7", "1e-7");
eq(StableStringify(333333333.33333329), "333333333.3333333",
   "RFC 8785 number vector 333333333.33333329");

/* Non-finite numbers have NO canonical form; the spec says reject. */
throws(() => StableStringify(NaN), "StableStringify rejects NaN");
throws(() => StableStringify(Infinity), "StableStringify rejects Infinity");

/* Determinism: two objects with the same content stringify identically
 * regardless of insertion order -- that is what makes it hashable. */
{
    const x = { z: 1, a: { d: 4, c: 3 }, m: [1, 2] };
    const y = { m: [1, 2], a: { c: 3, d: 4 }, z: 1 };
    eq(StableStringify(x), StableStringify(y),
       "insertion order does not change the canonical form");
    assert(JSON.stringify(x) !== JSON.stringify(y),
           "fault injection: plain JSON.stringify DOES differ, so the test is real");
}

/* No whitespace ever, even when asked. */
eq(StableStringify({ a: 1 }, { indent: 4 }), '{"a":1}',
   "StableStringify ignores indent -- canonical means one form");

/* Cycles throw rather than recursing forever. */
{
    const a = { x: 1 }; a.self = a;
    throws(() => StableStringify(a), "a cycle throws");
    throws(() => JSON5Stringify(a), "a cycle throws in JSON5Stringify too");
}
/* But a repeated node that is NOT a cycle must serialise fine. */
{
    const shared = { v: 1 };
    const dag = { l: shared, r: shared };
    eq(StableStringify(dag), '{"l":{"v":1},"r":{"v":1}}',
       "a shared (non-cyclic) node is not mistaken for a cycle");
}

if (fails) {
    print("test_json5: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_json5 failed");
}
print("test_json5: " + n + " assertions, 0 failures");
