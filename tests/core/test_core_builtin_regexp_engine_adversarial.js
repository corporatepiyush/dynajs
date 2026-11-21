// timeout: 120
// covers: c:src/builtins/builtin-regexp.c
// Adversarial edge suite for the WS-ENGINE touched subsystems:
// array-ext boundaries, object shape/length transitions, regexp compile
// malformed input, parser contextual-keyword boundaries, number formatting
// boundaries, JSON.rawJSON and Promise.try argument shapes.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_core_builtin_regexp_engine_adversarial");

let n = 0, failed = 0;
function ok(c, m) { n++; if (!c) { failed++; print("FAIL: " + m); } }
function eq(a, b, m) { ok(JSON.stringify(a) === JSON.stringify(b), m + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")"); }
function throwsAny(fn, m) {
    let e = null;
    try { fn(); } catch (err) { e = err; }
    ok(e !== null, m);
}

// Array-ext numeric boundaries
__kitS.section("Array-ext numeric boundaries", () => {

throwsAny(function () { new RegExp("("); }, "unbalanced group");

throwsAny(function () { new RegExp("["); }, "unbalanced class");

throwsAny(function () { new RegExp("a{2,1}"); }, "reversed quantifier");

throwsAny(function () { new RegExp("\\"); }, "trailing backslash");

ok(new RegExp("(?:a|b|c|d|e|f|g|h){2,4}").test("hgfedcba"), "nested alternatives with quantifier");

eq(new RegExp("^(a|b|c|d|e|f|g|h|i|j)+$").exec("jihgfedcba")[0], "jihgfedcba", "long alternation group");

eq(new RegExp("((((((((((a))))))))))").exec("a")[10], "a", "deep capture nesting");

eq("a".repeat(1000).replace(new RegExp("a{500}"), "b")[0], "b", "quantified repetition 500");

eq(/(?:)/.exec("")[0], "", "empty pattern");

eq(/\1(a)/.exec("aa")[0], "a", "forward backreference matches empty before participation");

eq(/(?<x>a)(?<y>b)\k<y>\k<x>/.exec("abba")[0], "abba", "two named groups cross backrefs");

throwsAny(function () { new RegExp("\\k<x>(?<y>a)"); }, "forward undefined name in unicode");

eq(new RegExp("a|".repeat(5000) + "a").exec("a")[0], "a", "alternation chain priority");

eq(new RegExp("(?:ab)+").exec("ababab")[0], "ababab", "repeated group");

eq(new RegExp("[\\s\\S]*").exec("x\ny")[0], "x\ny", "space class union");

setTimeout(function () {
    if (failed !== 0) throw new Error("engine_adversarial: " + failed + " failures of " + n);
    print("engine_adversarial: OK (" + n + " checks)");
}, 20);
});
__kitS.end();
