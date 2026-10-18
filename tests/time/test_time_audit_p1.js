// flags: --std
// Split from tests/test_audit_p1.js: the time sections.
import { RRule } from "dyna:time";

let n = 0, fails = 0;
function assert(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { n++; if (a !== b) { fails++; print("FAIL: " + m + " got " + JSON.stringify(a) + " want " + JSON.stringify(b)); } }
function throws(fn, re, m) {
    n++;
    try { fn(); fails++; print("FAIL: " + m + " did not throw"); }
    catch (e) {
        if (re && !re.test(String(e.message))) { fails++; print("FAIL: " + m + " wrong message: " + e.message); }
    }
}
{
    function E(y,m,d,h=0,mi=0,s=0){ return Date.UTC(y,m-1,d,h,mi,s)/1000; }
    const sec = new RRule({ freq: "SECONDLY", dtstart: E(2020,1,1) });
    throws(() => sec.between(E(2020,1,1), E(2022,1,1)), /budget exhausted/, "rrule between budget throw");
}
if (fails) { print("test_time_audit_p1: " + fails + " FAILED of " + n + " assertions"); throw new Error("test_time_audit_p1 failed"); }
print("test_time_audit_p1: " + n + " assertions, 0 failures");
