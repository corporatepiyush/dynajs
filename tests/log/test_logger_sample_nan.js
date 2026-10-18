// new Logger({ sample }) range-checks BEFORE the integer conversion: NaN
// failed neither "< 1" nor "> 2^32" and reached (int64_t)NaN, undefined
// behaviour found by the dyna:log input sweep under UBSan. Every non-integer
// must still be the documented TypeError; the san stage runs this suite.
import { Logger } from "dyna:log";
let n = 0, fails = 0;
function ok(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { ok(a === b, m + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")"); }
function err(f) { try { f(); return "no throw"; } catch (e) { return e.constructor.name + ": " + e.message; } }
for (const v of [NaN, Infinity, -Infinity, 0.5, 0, -1, 2 ** 32 + 1])
    eq(err(() => new Logger({ sample: v })), "TypeError: new Logger({ sample }): sample must be an integer 1..2^32", "Logger sample " + v);
ok(err(() => new Logger({ sample: 4 })) === "no throw", "Logger sample 4 (control)");

print("test_logger_sample_nan: " + n + " checks, " + fails + " failures");
if (fails) throw new Error(fails + " failures");
