// timeout: 120
// covers: c:src/builtins/builtin-number.c
// Adversarial edge suite for the WS-ENGINE touched subsystems:
// array-ext boundaries, object shape/length transitions, regexp compile
// malformed input, parser contextual-keyword boundaries, number formatting
// boundaries, JSON.rawJSON and Promise.try argument shapes.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_core_builtin_number_engine_adversarial");

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

ok(Number.isFinite((1e308).round(-308)), "round -308 finite");

ok(Number.isFinite((1e100).ceil(20)), "ceil high precision finite");

setTimeout(function () {
    if (failed !== 0) throw new Error("engine_adversarial: " + failed + " failures of " + n);
    print("engine_adversarial: OK (" + n + " checks)");
}, 20);
});
__kitS.end();
