// covers: dyna:decimal.Decimal.div
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_decimal_decimal_div_strict");
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

__kitS.section("div `mode` refuses (the real key is `rounding`)", () => {
throws(() => d.div(3, { mode: "down" }), /unknown option "mode"/,
       "div `mode` refuses (the real key is `rounding`)");
});

__kitS.section("div with the full honest bag still works", () => {
eq(d.div(3, { precision: 10, rounding: "down" }).toString(), "0.5",
   "div with the full honest bag still works");
});

__kitS.section("default-precision div unchanged", () => {
eq(d.div(3).toString(), "0.5", "default-precision div unchanged");
});
__kitS.end();

print((pass + fail) + " asserts: " + pass + " pass, " + fail + " fail");
if (fail) throw new Error("test_decimal_strict: " + fail + " failures");
