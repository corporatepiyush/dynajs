// covers: dyna:decimal.Decimal.round
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_decimal_decimal_round_strict");
import { Decimal } from "dyna:decimal";

let pass = 0, fail = 0;
const ok = (c, m) => { if (c) pass++; else { fail++; print("  FAIL: " + m); } };
const eq = (a, b, m) => ok(a === b, m + " (got " + a + ", want " + b + ")");
const throws = (fn, rx, m) => {
    try { fn(); fail++; print("  FAIL: " + m + " (did not throw)"); }
    catch (e) {
        if (!(e instanceof TypeError)) { fail++; print("  FAIL: " + m + " wrong kind " + (e && e.constructor.name) + " [" + e.message + "]"); return; }
        if (rx && !rx.test(e.message)) { fail++; print("  FAIL: " + m + " message [" + e.message + "]"); return; }
        pass++;
    }
};

const d = new Decimal("1.5");

__kitS.section("round `rnd` refuses", () => {
throws(() => d.round(2, { rnd: "up" }), /unknown option "rnd"/,
       "round `rnd` refuses");
});

__kitS.section("the bare-string rounding form bypasses the bag check", () => {
eq(d.round(1, "down").toString(), "1.5", "the bare-string rounding form bypasses the bag check");
});

__kitS.section("plain round unchanged", () => {
eq(new Decimal("2").round(0).toString(), "2", "plain round unchanged");
});
__kitS.end();

print((pass + fail) + " asserts: " + pass + " pass, " + fail + " fail");
if (fail) throw new Error("test_decimal_strict: " + fail + " failures");
