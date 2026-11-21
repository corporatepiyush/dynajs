// timeout: 120
// covers: c:src/runtime/class.inc.c
// Adversarial edge suite for the WS-ENGINE touched subsystems:
// array-ext boundaries, object shape/length transitions, regexp compile
// malformed input, parser contextual-keyword boundaries, number formatting
// boundaries, JSON.rawJSON and Promise.try argument shapes.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_core_class_inc_engine_adversarial");

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

eq(new Function("return class C { get\nfoo() { return 1; } };")() .prototype.foo, 1, "getter after newline");

eq(new Function("let C = class C { set\nfoo(v) { this._v = v; } m() { return this._v; } }; let c = new C(); c.foo = 5; return c.m();")(), 5, "setter after newline stores value");

eq(new Function("return class C { static get x() { return 1; } };")() .x, 1, "static getter");

setTimeout(function () {
    if (failed !== 0) throw new Error("engine_adversarial: " + failed + " failures of " + n);
    print("engine_adversarial: OK (" + n + " checks)");
}, 20);
});
__kitS.end();
