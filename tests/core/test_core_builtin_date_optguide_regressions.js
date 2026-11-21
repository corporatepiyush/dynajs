// covers: c:src/builtins/builtin-date.c
// Split from tests/test_optguide_regressions.js: the core sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_core_builtin_date_optguide_regressions");
export {};

let pass = 0, fail = 0;
function ok(c, m) { if (c) { pass++; } else { fail++; print("FAIL: " + m); } }
function eq(a, b, m) { ok(Object.is(a, b) || a === b, m + "  (got " + a + ", want " + b + ")"); }
function eqj(a, b, m) {
    const x = JSON.stringify(a), y = JSON.stringify(b);
    ok(x === y, m + "  (got " + x + ", want " + y + ")");
}

__kitS.section("Thu, 01 Jan 1970 00:00:00 GMT", () => {
    const base = "Thu, 01 Jan 1970 00:00:00 GMT";
    eq(Date.parse(base), 0, "the plain form parses to the epoch");
    for (const pad of [1, 50, 120, 500, 5000]) {
        eq(Date.parse(" ".repeat(pad) + base), 0,
           "leading whitespace x" + pad + " does not change the timestamp");
    }
    eq(Date.parse("\t\n\r\f\v " + base), 0, "every whitespace form is skipped");
    ok(Number.isNaN(Date.parse(base + "x".repeat(500))), "an over-long input is NaN");
    ok(Number.isNaN(Date.parse("x".repeat(500))), "long garbage is NaN");
    eq(new Date(8.64e15).toISOString(), "+275760-09-13T00:00:00.000Z", "max Date");
    eq(new Date(-8.64e15).toISOString(), "-271821-04-20T00:00:00.000Z", "min Date");
    ok(Number.isNaN(new Date(8.64e15 + 1).getTime()), "one past the range is not a Date");
    for (const t of [0, 1, -1, 1e12, -1e12, 8.64e15, -8.64e15]) {
        eq(Date.parse(new Date(t).toISOString()), t, "toISOString round-trips at " + t);
    }
});
__kitS.end();
print("test_core_optguide_regressions: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error(fail + " failures");
