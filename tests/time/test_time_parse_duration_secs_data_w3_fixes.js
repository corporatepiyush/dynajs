// timeout: 180
// covers: dyna:time.parseDurationSecs
// Split from tests/test_data_w3_fixes.js: the time sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_time_parse_duration_secs_data_w3_fixes");
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

__kitS.section("COMPAT-262: parseDurationSecs is correctly rounded", () => {
eq(parseDurationSecs("1577960769301841103ns"), 1577960769.301841,
   "COMPAT-262: parseDurationSecs is correctly rounded");
});

__kitS.section("COMPAT-262: seconds counterexample", () => {
eq(parseDurationSecs("4581490823437043614ns"), 4581490823.437043,
   "COMPAT-262: seconds counterexample");
if (fails === 0) print("test_time_data_w3_fixes: all " + n + " checks passed");
else print("test_time_data_w3_fixes: " + fails + " FAILED of " + n);
});
__kitS.end();
