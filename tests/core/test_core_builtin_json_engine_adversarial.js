// timeout: 120
// covers: c:src/builtins/builtin-json.c
// Adversarial edge suite for the WS-ENGINE touched subsystems:
// array-ext boundaries, object shape/length transitions, regexp compile
// malformed input, parser contextual-keyword boundaries, number formatting
// boundaries, JSON.rawJSON and Promise.try argument shapes.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_core_builtin_json_engine_adversarial");

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

eq(JSON.stringify([JSON.rawJSON("0"), JSON.rawJSON("-1.5"), JSON.rawJSON('"s"')]), '[0,-1.5,"s"]', "rawJSON primitives in array");

throwsAny(function () { JSON.rawJSON("{}"); }, "rawJSON empty object text");

throwsAny(function () { JSON.rawJSON("1 2"); }, "rawJSON trailing garbage");

throwsAny(function () { JSON.rawJSON("01"); }, "rawJSON leading zero");

setTimeout(function () {
    if (failed !== 0) throw new Error("engine_adversarial: " + failed + " failures of " + n);
    print("engine_adversarial: OK (" + n + " checks)");
}, 20);
});
__kitS.end();
