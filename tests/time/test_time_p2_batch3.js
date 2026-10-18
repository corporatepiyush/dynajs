// Split from tests/test_p2_batch3.js: the time sections.
import { RRule } from "dyna:time";

let n = 0, fails = 0;
function assert(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function throws(fn, re, m) {
    n++;
    try { fn(); fails++; print("FAIL: " + m + " did not throw"); }
    catch (e) { if (re && !re.test(String(e.message))) { fails++; print("FAIL: " + m + " wrong msg: " + e.message); } }
}
{
    const r1 = new RRule({ freq: "WEEKLY", dtstart: new Date(Date.UTC(2020, 0, 1)), wkst: 1 });
    assert(r1 !== null, "RRule wkst 1 ok");
    const r2 = new RRule({ freq: "WEEKLY", dtstart: new Date(Date.UTC(2020, 0, 1)), wkst: "MO" });
    assert(r2 !== null, "RRule wkst MO ok");
    throws(() => new RRule({ freq: "WEEKLY", wkst: 7 }), /wkst/, "RRule wkst 7 rejects");
    throws(() => new RRule({ freq: "WEEKLY", wkst: "XX" }), /wkst/, "RRule wkst XX rejects");
}
if (fails) { print("test_time_p2_batch3: " + fails + " FAILED of " + n + " assertions"); throw new Error("test_time_p2_batch3 failed"); }
print("test_time_p2_batch3: " + n + " assertions, 0 failures");
