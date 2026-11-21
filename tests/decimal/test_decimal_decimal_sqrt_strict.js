// covers: dyna:decimal.Decimal.sqrt
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_decimal_decimal_sqrt_strict");
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

__kitS.section("sqrt ignores a KNOWN `rounding` (default precision, 34 digits)", () => {
eq(new Decimal("2").sqrt(0, { rounding: "up" }).toString(), "1.414213562373095048801688724209698",
   "sqrt ignores a KNOWN `rounding` (default precision, 34 digits)");
});

__kitS.section("sqrt `digits` refuses (the real key is `precision`)", () => {
throws(() => d.sqrt({ digits: 30 }), /unknown option "digits"/,
       "sqrt `digits` refuses (the real key is `precision`)");
});

__kitS.section("sqrt with precision still sizes the result", () => {
eq(new Decimal("2").sqrt({ precision: 10 }).toString(), "1.414213562",
   "sqrt with precision still sizes the result");
});
__kitS.end();

print((pass + fail) + " asserts: " + pass + " pass, " + fail + " fail");
if (fail) throw new Error("test_decimal_strict: " + fail + " failures");
