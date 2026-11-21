// covers: dyna:time.RRule
// Split from tests/test_audit_p1.js: the time sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_time_r_rule_audit_p1");
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

__kitS.section("rrule between budget throw", () => {
    function E(y,m,d,h=0,mi=0,s=0){ return Date.UTC(y,m-1,d,h,mi,s)/1000; }
    const sec = new RRule({ freq: "SECONDLY", dtstart: E(2020,1,1) });
    throws(() => sec.between(E(2020,1,1), E(2022,1,1)), /budget exhausted/, "rrule between budget throw");
});
__kitS.end();
if (fails) { print("test_time_audit_p1: " + fails + " FAILED of " + n + " assertions"); throw new Error("test_time_audit_p1 failed"); }
print("test_time_audit_p1: " + n + " assertions, 0 failures");
