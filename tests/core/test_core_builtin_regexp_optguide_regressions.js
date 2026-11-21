// covers: c:src/builtins/builtin-regexp.c
// Split from tests/test_optguide_regressions.js: the core sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_core_builtin_regexp_optguide_regressions");
export {};

let pass = 0, fail = 0;
function ok(c, m) { if (c) { pass++; } else { fail++; print("FAIL: " + m); } }
function eq(a, b, m) { ok(Object.is(a, b) || a === b, m + "  (got " + a + ", want " + b + ")"); }
function eqj(a, b, m) {
    const x = JSON.stringify(a), y = JSON.stringify(b);
    ok(x === y, m + "  (got " + x + ", want " + y + ")");
}

__kitS.section("Hello World", () => {
    eqj("Hello World".match(/[a-z]+/gi), ["Hello", "World"], "range_i fold matches both cases");
    eqj("aAbB".match(/[ab]/gi), ["a", "A", "b", "B"], "char class fold is exact");
    eq("XyZ".replace(/[a-z]/gi, "."), "...", "replace over a folded class");
    eq("straße".match(/STRASSE/i), null, "the fold does not overreach");
});
__kitS.end();
print("test_core_optguide_regressions: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error(fail + " failures");
