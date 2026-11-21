// timeout: 60
// covers: dyna:time.Duration
// R5-5: Duration component bounds are exact: every field accepts +/-1e12 and
// refuses one past it with a RangeError; a folded value whose three parts
// disagree in sign constructs but toString() refuses it (ISO 8601 has no
// mixed-sign form). The documented clause "|value| <= 1e12" is checked against
// dynajs.d.ts by tests/api/test_contract_docreqs.js, whose grep requires the
// literal phrase.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_time_duration_bounds");
import { Duration } from "dyna:time";

let n = 0, fails = 0;
function ok(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function throws(fn, rx, m) {
    n++;
    try { fn(); fails++; print("FAIL: " + m + " (did not throw)"); }
    catch (e) {
        if (!(e instanceof RangeError)) { fails++; print("FAIL: " + m + " wrong kind " + e); return; }
        if (rx && !rx.test(e.message)) { fails++; print("FAIL: " + m + " message [" + e.message + "]"); }
    }
}

const FIELDS = ["years", "months", "weeks", "days", "hours", "minutes", "seconds", "milliseconds"];
const MAX = 1000000000000, MAXP = MAX + 1;

for (const f of FIELDS) {
    ok(new Duration({ [f]: MAX }) instanceof Duration, f + " = 1e12 constructs");
    ok(new Duration({ [f]: -MAX }) instanceof Duration, f + " = -1e12 constructs");
    throws(() => new Duration({ [f]: MAXP }),
        /exceeds 1000000000000/, f + " = 1e12+1 refuses");
    throws(() => new Duration({ [f]: -MAXP }),
        /exceeds 1000000000000/, f + " = -1e12-1 refuses");
}

ok(new Duration({ hours: MAX, minutes: MAX, seconds: MAX, milliseconds: MAX }) instanceof Duration,
    "four fields at +1e12 fold together and construct");

ok(new Duration({ years: MAX, months: MAX }) instanceof Duration,
    "years and months at +1e12 fold together and construct");

__kitS.section("a mixed-sign Duration constructs", () => {
    const mixed = new Duration({ months: 1, milliseconds: -1 });
    ok(mixed instanceof Duration, "a mixed-sign Duration constructs");
    throws(() => String(mixed), /mixed-sign/, "toString refuses months:+1 ms:-1");
    throws(() => String(new Duration({ years: -1, days: 1 })), /mixed-sign/,
        "toString refuses years:-1 days:+1 (folds to months:-12 days:+1)");
    throws(() => String(new Duration({ months: 1, days: -1 })), /mixed-sign/,
        "toString refuses months:+1 days:-1");
    ok(mixed.sign === 1 && mixed.blank === false,
        "a mixed-sign value still reports its sign and non-blank state");
    const unary = new Duration({ milliseconds: -5 });
    ok(String(unary) === "-PT0.005S", "a uniform negative still prints (" + String(unary) + ")");
    ok(String(new Duration({ milliseconds: 5 })) === "PT0.005S", "a uniform positive still prints");
    ok(String(new Duration({})) === "P0D", "the empty Duration prints P0D");
    ok(new Duration({ months: 1, milliseconds: -1 }).sign === 1,
        "mixed sign follows the months part (documented sign rule)");
});
__kitS.end();

if (fails === 0) print("test_duration_bounds: all " + n + " checks passed");
else {
    print("test_duration_bounds: " + fails + " FAILED of " + n);
    throw new Error("test_duration_bounds: " + fails + " failures");
}
