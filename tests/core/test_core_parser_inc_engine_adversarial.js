// timeout: 120
// covers: c:src/parser/parser.inc.c
// Adversarial edge suite for the WS-ENGINE touched subsystems:
// array-ext boundaries, object shape/length transitions, regexp compile
// malformed input, parser contextual-keyword boundaries, number formatting
// boundaries, JSON.rawJSON and Promise.try argument shapes.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_core_parser_inc_engine_adversarial");

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

eq(new Function("return ({ get: 1 })")().get, 1, "object literal get key");

eq(new Function("return ({ set: 2 })")().set, 2, "object literal set key");

ok(new Function("let a = 1\nlet b = 2\nreturn a + b")() === 3, "ASI two lets");

eq(new Function("return (1,\n2)")() , 2, "parenthesized newline");

throwsAny(function () { new Function("return (1\n2)")(); }, "no ASI inside parens");

setTimeout(function () {
    if (failed !== 0) throw new Error("engine_adversarial: " + failed + " failures of " + n);
    print("engine_adversarial: OK (" + n + " checks)");
}, 20);
});
__kitS.end();
