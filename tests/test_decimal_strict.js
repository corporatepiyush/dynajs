// flags: --std
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

eq(d.add(1, { precision: 30 }).toString(), "2.5", "add ignores a KNOWN `precision`");
eq(d.mul(2, { rounding: "down" }).toString(), "3", "mul ignores a KNOWN `rounding`");
eq(d.sub(0.5, { precision: 5, rounding: "ceil" }).toString(), "1", "sub ignores both known keys");
eq(new Decimal("2").sqrt(0, { rounding: "up" }).toString(), "1.414213562373095048801688724209698",
   "sqrt ignores a KNOWN `rounding` (default precision, 34 digits)");
eq(d.divmod(0.5, { precision: 10 }).length, 2, "divmod accepts the known keys it ignores");

throws(() => d.add(1, { precison: 30 }), /unknown option "precison" \(valid: precision, rounding\)/,
       "add typo'd key refuses with the full valid set");
throws(() => d.div(3, { mode: "down" }), /unknown option "mode"/,
       "div `mode` refuses (the real key is `rounding`)");
throws(() => d.pow(2, { exp: 2 }), /unknown option "exp"/,
       "pow `exp` refuses");
throws(() => d.sqrt({ digits: 30 }), /unknown option "digits"/,
       "sqrt `digits` refuses (the real key is `precision`)");
throws(() => d.log10({ radix: 10 }), /unknown option "radix"/,
       "log10 `radix` refuses");
throws(() => d.round(2, { rnd: "up" }), /unknown option "rnd"/,
       "round `rnd` refuses");
throws(() => d.divmod(2, { foo: 1 }), /unknown option "foo"/,
       "divmod's ignored bag is still checked");

eq(d.div(3, { precision: 10, rounding: "down" }).toString(), "0.5",
   "div with the full honest bag still works");
eq(new Decimal("2").sqrt({ precision: 10 }).toString(), "1.414213562",
   "sqrt with precision still sizes the result");
eq(d.round(1, "down").toString(), "1.5", "the bare-string rounding form bypasses the bag check");
eq(new Decimal("2").round(0).toString(), "2", "plain round unchanged");
eq(d.div(3).toString(), "0.5", "default-precision div unchanged");

print((pass + fail) + " asserts: " + pass + " pass, " + fail + " fail");
if (fail) throw new Error("test_decimal_strict: " + fail + " failures");
