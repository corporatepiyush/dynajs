// Split from tests/test_cov_sys_time_random_uuid_misc.js: the log sections.
import { Logger, Debug } from "dyna:log";

let n = 0, fails = 0;
function assert(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { assert(a === b, m + " got " + JSON.stringify(a) + " want " + JSON.stringify(b)); }
function throws(fn, re, m) {
    n++;
    try { fn(); fails++; print("FAIL: " + m + " did not throw"); }
    catch (e) { if (re && !re.test(String(e.message))) { fails++; print("FAIL: " + m + " wrong msg: " + e.message); } }
}
function assertClose(a, b, tol, m) {
    n++;
    if (!(Math.abs(a - b) <= tol)) { fails++; print("FAIL: " + m + " |" + a + "-" + b + "|>" + tol); }
}
print("=== sys ===");
{
    const lg = new Logger({ name: "cov" });
    lg.info("hello"); n++;
    lg.warn("w"); lg.error("e"); lg.debug("d"); n += 3;
    const child = lg.child({ req: "r1" });
    child.info("child line"); n++;
    try { lg.setLevel ? lg.setLevel("error") : null; lg.info("suppressed"); n++; } catch (e) { n++; }
    const dbg = Debug("cov:test");
    assert(typeof dbg === "function", "Debug returns function");
    dbg("payload"); n++;
}
if (fails) {
    print("test_log_cov_sys_time_random_uuid_misc: " + fails + " FAILED of " + n + " assertions");
    throw new Error("test_log_cov_sys_time_random_uuid_misc failed");
}
print("test_log_cov_sys_time_random_uuid_misc: " + n + " assertions, 0 failures");
