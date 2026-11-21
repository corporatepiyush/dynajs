// timeout: 180
// covers: dyna:time.parseDurationMs
// Split from tests/test_data_w3_fixes.js: the time sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_time_parse_duration_ms_data_w3_fixes");
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

__kitS.section("COMPAT-262: parseDurationMs is correctly rounded (was 1 ulp high)", () => {
eq(parseDurationMs("2784249659845191438ns"), 2784249659845.1914,
   "COMPAT-262: parseDurationMs is correctly rounded (was 1 ulp high)");
});

__kitS.section("COMPAT-262: negative ns is correctly rounded", () => {
eq(parseDurationMs("-8531699129511414324ns"), -8531699129511.414,
   "COMPAT-262: negative ns is correctly rounded");
});

__kitS.section("COMPAT-262: a third counterexample", () => {
eq(parseDurationMs("8785356068037452444ns"), 8785356068037.452,
   "COMPAT-262: a third counterexample");
});

__kitS.section("COMPAT-262: the documented exact case still holds", () => {
eq(parseDurationMs("200000h"), 7.2e11, "COMPAT-262: the documented exact case still holds");
if (fails === 0) print("test_time_data_w3_fixes: all " + n + " checks passed");
else print("test_time_data_w3_fixes: " + fails + " FAILED of " + n);
});
__kitS.end();
