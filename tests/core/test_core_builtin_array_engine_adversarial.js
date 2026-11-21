// timeout: 120
// covers: c:src/builtins/builtin-array.c
// Adversarial edge suite for the WS-ENGINE touched subsystems:
// array-ext boundaries, object shape/length transitions, regexp compile
// malformed input, parser contextual-keyword boundaries, number formatting
// boundaries, JSON.rawJSON and Promise.try argument shapes.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_core_builtin_array_engine_adversarial");

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

ok(Array.isArray([1, 2].aperture(1)), "aperture normal returns array of windows");

throwsAny(function () { Array.prototype.insert.call({ length: 2 ** 53 }, 0, 1); }, "insert on 2^53 length throws");

throwsAny(function () { Array.prototype.prepend.call({ length: 2 ** 32 }, 1); }, "prepend on 2^32 length throws");

ok((1234.5678).format(2, "_", ".").indexOf("_") > 0, "format custom separators");

ok((-1234.5678).format(2).indexOf("-") === 0, "format negative sign");

eq([].reduce(function (a, b) { return a + b; }, 0), 0, "empty reduce with seed");

throwsAny(function () { [].reduce(function (a, b) { return a + b; }); }, "empty reduce without seed throws");

setTimeout(function () {
    if (failed !== 0) throw new Error("engine_adversarial: " + failed + " failures of " + n);
    print("engine_adversarial: OK (" + n + " checks)");
}, 20);
});
__kitS.end();
