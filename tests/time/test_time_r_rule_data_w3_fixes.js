// timeout: 180
// covers: dyna:time.RRule
// Split from tests/test_data_w3_fixes.js: the time sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_time_r_rule_data_w3_fixes");
import { TOML } from "dyna:config";
import { RRule, Duration, date, parseDurationMs, parseDurationSecs } from "dyna:time";
import { setNativeMemoryLimit } from "dyna:sys";
import { cpuNow } from "../lib/cpuclock.js";

let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { check(a === b, m + " -- got " + JSON.stringify(a) + ", want " + JSON.stringify(b)); }
async function throws(fn, m) { n++; try { await fn(); fails++; print("FAIL: " + m + " (did not throw)"); } catch (e) { } }
function peq(text, expected, m) {
    n++;
    let got;
    try { got = TOML.parse(text).s; }
    catch (e) { fails++; print("FAIL: " + m + " (threw " + e + ")"); return; }
    check(got === expected, m + " -- got " + JSON.stringify(got) + ", want " + JSON.stringify(expected));
}

__kitS.section("OPT-057: RRule allocations count against the native limit", () => {
    const keep = [];
    let threwRR = false;
    setNativeMemoryLimit(1 << 20);
    try { for (let i = 0; i < 200000; i++) keep.push(new RRule({ freq: "DAILY" })); }
    catch (e) { threwRR = true; }
    keep.length = 0;
    const keep2 = [];
    let threwDur = false;
    setNativeMemoryLimit(1 << 20);
    try { for (let i = 0; i < 200000; i++) keep2.push(new Duration({ days: 1 })); }
    catch (e) { threwDur = true; }
    keep2.length = 0;
    setNativeMemoryLimit(0);
    check(threwRR, "OPT-057: RRule allocations count against the native limit");
    check(threwDur, "OPT-058: Duration allocations count against the native limit");
});
__kitS.end();
if (fails === 0) print("test_time_data_w3_fixes: all " + n + " checks passed");
else print("test_time_data_w3_fixes: " + fails + " FAILED of " + n);
