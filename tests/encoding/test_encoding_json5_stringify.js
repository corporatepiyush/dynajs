// covers: dyna:encoding.JSON5Stringify
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_encoding_json5_stringify");
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

eq(JSON5Stringify({ a: 1 }), '{"a":1}', "stringify object");

eq(JSON5Stringify([1, 2]), "[1,2]", "stringify array");

eq(JSON5Stringify("hi"), '"hi"', "stringify string");

eq(JSON5Stringify(null), "null", "stringify null");

eq(JSON5Stringify(true), "true", "stringify true");

eq(JSON5Stringify(NaN), "NaN", "JSON5Stringify prints NaN");

eq(JSON5Stringify(Infinity), "Infinity", "JSON5Stringify prints Infinity");

eq(JSON5Stringify(-Infinity), "-Infinity", "JSON5Stringify prints -Infinity");

eq(JSON5Stringify({ a: 1 }, { indent: 2 }), '{\n  "a": 1\n}', "indent");

eq(JSON5Stringify({ a: undefined, b: 1 }), '{"b":1}', "undefined members are omitted");

eq(JSON5Stringify([undefined]), "[null]", "undefined in an array is null");

eq(JSON5Stringify({ a: "\n\t\"" }), '{"a":"\\n\\t\\""}', "control chars escape");

__kitS.section("a cycle throws", () => {
    const a = { x: 1 }; a.self = a;
    throws(() => StableStringify(a), "a cycle throws");
    throws(() => JSON5Stringify(a), "a cycle throws in JSON5Stringify too");
});
__kitS.end();

throws(() => JSON5Stringify(10n), "JSON5Stringify rejects BigInt");

eq(JSON5Stringify({ f: function () {}, g: 1 }), '{"g":1}', "function member omitted (json5)");

if (fails) {
    print("test_json5: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_json5 failed");
}
print("test_json5: " + n + " assertions, 0 failures");
