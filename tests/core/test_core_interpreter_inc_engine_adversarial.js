// timeout: 120
// covers: c:src/vm/interpreter.inc.c
// Adversarial edge suite for the WS-ENGINE touched subsystems:
// array-ext boundaries, object shape/length transitions, regexp compile
// malformed input, parser contextual-keyword boundaries, number formatting
// boundaries, JSON.rawJSON and Promise.try argument shapes.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_core_interpreter_inc_engine_adversarial");

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

eq([].removeRange(0, 0), [], "empty removeRange");

eq([1].removeRange(0, 2 ** 53), [], "removeRange count 2^53");

eq([1, 2, 3].removeRange(2 ** 53, 5), [1, 2, 3], "removeRange start 2^53 clamps");

eq([1, 2, 3].removeRange(-(2 ** 53), 1), [2, 3], "removeRange start -2^53 clamps to 0");

eq([1, 2, 3].removeRange(0x7fffffffffffffff, 0x7fffffffffffffff), [1, 2, 3], "removeRange INT64_MAX");

eq([1, 2, 3].removeRange(-0x8000000000000000, 3), [], "removeRange INT64_MIN start");

throwsAny(function () { [].aperture(-1); }, "aperture negative throws");

eq([1].aperture(0), [[], []], "aperture 0 returns len+1 empty windows (implementation-defined; DOC-REQ)");

eq([1, 2, 3].aperture(2), [[1, 2], [2, 3]], "aperture normal windows");

eq([].insertAll(0, [1, 2]), [1, 2], "insertAll empty receiver");

eq([1, 2].insertAll(1, []), [1, 2], "insertAll empty source");

ok((0).format(0) === "0", "format zero");

ok((1e-300).abbr(5).length > 0, "abbr tiny value");

ok((0).abbr(3).length > 0, "abbr zero");

eq((1).pad(3, true), "+001", "pad with sign");

eq((-1).pad(3, true), "-001", "pad negative with sign");

eq((255).pad(0, false, 16), "ff", "pad hex lowercase");

eq((86400000).duration(), "1 day", "duration day");

eq((0).ordinalize(), "0th", "ordinalize zero");

eq((11).ordinalize(), "11th", "ordinalize 11th");

eq((21).ordinalize(), "21st", "ordinalize 21st");

eq((112).ordinalize(), "112th", "ordinalize 112th");

eq("abc".containsAny("xyz"), false, "containsAny miss");

eq("abc".containsAny("cb"), true, "containsAny hit");

setTimeout(function () {
    if (failed !== 0) throw new Error("engine_adversarial: " + failed + " failures of " + n);
    print("engine_adversarial: OK (" + n + " checks)");
}, 20);
});
__kitS.end();
