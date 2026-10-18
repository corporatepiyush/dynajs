// Split from tests/test_optguide_regressions.js: the time sections.
import { Duration } from "dyna:time";

let pass = 0, fail = 0;
function ok(c, m) { if (c) { pass++; } else { fail++; print("FAIL: " + m); } }
function eq(a, b, m) { ok(Object.is(a, b) || a === b, m + "  (got " + a + ", want " + b + ")"); }
function eqj(a, b, m) {
    const x = JSON.stringify(a), y = JSON.stringify(b);
    ok(x === y, m + "  (got " + x + ", want " + y + ")");
}
{
    eq(new Duration({ months: 1, days: 10 }).toString(), "P1M10D", "all-positive duration");
    eq(new Duration({ months: -1, days: -10 }).toString(), "-P1M10D", "all-negative duration");
    eq(new Duration({ months: 0, days: 0 }).toString(), "P0D", "zero duration");
    for (const [mo, d] of [[1, -10], [-1, 10], [-5, 3], [7, -2]]) {
        let threw = null;
        try { new Duration({ months: mo, days: d }).toString(); } catch (e) { threw = e; }
        ok(threw && /mixed-sign/.test(threw.message),
           "mixed-sign (" + mo + "," + d + ") is refused, never rendered");
    }
    // SEC-246: Duration components are bounded at |value| <= 1e12 (the
    // documented contract); this block's original 2^53-1 probes predate that
    // bound. Keep the safety intent with the largest ADMITTED magnitudes.
    let s = null;
    try { s = new Duration({ months: -1000000000000, days: 1000000000000 }).toString(); }
    catch (e) { s = "threw"; }
    ok(s === "threw",
       "the largest admitted mixed-sign duration is refused by toString, never corrupting the stack");
    ok(new Duration({ days: 1000000000000 }).toString().length < 90,
       "the largest admitted single-component duration stays inside the buffer");
    let over = null;
    try { new Duration({ days: 1000000000001 }); } catch (e) { over = e; }
    ok(over instanceof RangeError,
       "a component above the documented 1e12 bound is refused");
}
print("test_time_optguide_regressions: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error(fail + " failures");
