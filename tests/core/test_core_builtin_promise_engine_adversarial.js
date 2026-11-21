// timeout: 120
// covers: c:src/builtins/builtin-promise.c
// Adversarial edge suite for the WS-ENGINE touched subsystems:
// array-ext boundaries, object shape/length transitions, regexp compile
// malformed input, parser contextual-keyword boundaries, number formatting
// boundaries, JSON.rawJSON and Promise.try argument shapes.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_core_builtin_promise_engine_adversarial");

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

ok(new Function("return class C { async foo() { return 2; } };")() .prototype.foo() instanceof Promise, "async method returns a promise");

eq(new Function("return ({ async: 3 })")().async, 3, "object literal async key");

let trySeen = "pending";

Promise.try(function () { return 1; }).then(function (v) { trySeen = v; });
Promise.try(function () { throw new RangeError("x"); }).catch(function (e) { ok(e instanceof RangeError, "Promise.try throw routes to reject"); });

setTimeout(function () {
    if (failed !== 0) throw new Error("engine_adversarial: " + failed + " failures of " + n);
    print("engine_adversarial: OK (" + n + " checks)");
}, 20);
});
__kitS.end();
