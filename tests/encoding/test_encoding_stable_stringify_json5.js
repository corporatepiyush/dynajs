// covers: dyna:encoding.StableStringify
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_encoding_stable_stringify_json5");
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

eq(StableStringify({ b: 1, a: 2 }), '{"a":2,"b":1}', "keys are sorted");

eq(StableStringify({ a: 1, A: 2 }), '{"A":2,"a":1}', "uppercase sorts before lowercase");

eq(StableStringify({ "10": 1, "2": 2 }), '{"10":1,"2":2}',
   "numeric-looking keys sort as strings, not numbers");

eq(StableStringify({}), "{}", "empty object");

eq(StableStringify([]), "[]", "empty array");

eq(StableStringify([3, 1, 2]), "[3,1,2]", "array order is NOT sorted");

__kitS.section("Euro Sign", () => {
    const input = { "\u20AC": "Euro Sign", "\r": "Carriage Return",
                    "\n": "Newline", "1": "One",
                    "\u0080": "Control", "\u00F6": "Latin Small O With Diaeresis",
                    "\u20ACa": "?", "A": "A" };
    const out = StableStringify(input);
    const keysInOrder = [];
    for (const m of out.matchAll(/"((?:[^"\\]|\\.)*)":/g)) keysInOrder.push(m[1]);
    assert(keysInOrder.length === 8, "RFC 8785 example emitted all 8 keys (got "
           + keysInOrder.length + ")");
    const decoded = keysInOrder.map(k => JSON.parse('"' + k + '"'));
    let sorted = true;
    for (let i = 1; i < decoded.length; i++)
        if (!(decoded[i - 1] < decoded[i])) { sorted = false; break; }
    assert(sorted, "RFC 8785 keys come out in ascending UTF-16 order: "
           + JSON.stringify(decoded));
});

__kitS.section("an astral key sorts BEFORE U+FF01 (UTF-16 order, not code point)", () => {
    const out = StableStringify({ "\uFF01": 1, "\u{1f600}": 2 });
    assert(out.indexOf("\u{1f600}") < out.indexOf("\uFF01"),
           "an astral key sorts BEFORE U+FF01 (UTF-16 order, not code point)");
    assert("\u{1f600}" < "\uFF01", "JS string < agrees: UTF-16 order");
    assert(0x1f600 > 0xff01, "code point order would say the opposite");
});

eq(StableStringify(1), "1", "integer 1");

eq(StableStringify(1.0), "1", "1.0 prints as 1");

eq(StableStringify(-0), "0", "negative zero canonicalises to 0");

eq(StableStringify(0.1), "0.1", "0.1");

eq(StableStringify(1e21), "1e+21", "1e21 keeps exponent form");

eq(StableStringify(1e-7), "1e-7", "1e-7");

eq(StableStringify(333333333.33333329), "333333333.3333333",
   "RFC 8785 number vector 333333333.33333329");

throws(() => StableStringify(NaN), "StableStringify rejects NaN");

throws(() => StableStringify(Infinity), "StableStringify rejects Infinity");

__kitS.section("insertion order does not change the canonical form", () => {
    const x = { z: 1, a: { d: 4, c: 3 }, m: [1, 2] };
    const y = { m: [1, 2], a: { c: 3, d: 4 }, z: 1 };
    eq(StableStringify(x), StableStringify(y),
       "insertion order does not change the canonical form");
    assert(JSON.stringify(x) !== JSON.stringify(y),
           "fault injection: plain JSON.stringify DOES differ, so the test is real");
});

eq(StableStringify({ a: 1 }, { indent: 4 }), '{"a":1}',
   "StableStringify ignores indent -- canonical means one form");

__kitS.section("a shared (non-cyclic) node is not mistaken for a cycle", () => {
    const shared = { v: 1 };
    const dag = { l: shared, r: shared };
    eq(StableStringify(dag), '{"l":{"v":1},"r":{"v":1}}',
       "a shared (non-cyclic) node is not mistaken for a cycle");
});
__kitS.end();

throws(() => StableStringify(10n), "StableStringify rejects BigInt");

eq(StableStringify({ a: 1 }), '{"a":1}', "stringify still works after the BigInt guard");

eq(StableStringify({ f: function () {}, g: 1 }), '{"g":1}', "function member omitted");

eq(StableStringify([function () {}]), "[null]", "function in array is null");

if (fails) {
    print("test_json5: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_json5 failed");
}
print("test_json5: " + n + " assertions, 0 failures");
