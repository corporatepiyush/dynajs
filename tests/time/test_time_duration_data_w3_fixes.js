// timeout: 180
// covers: dyna:time.Duration
// Split from tests/test_data_w3_fixes.js: the time sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_time_duration_data_w3_fixes");
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

await __kitS.section("SEC-246: a huge Duration year is refused", async () => {
await throws(() => new Duration({ years: 2 ** 62 }), "SEC-246: a huge Duration year is refused");
});

await __kitS.section("SEC-246: huge Duration milliseconds are refused", async () => {
await throws(() => new Duration({ milliseconds: 2 ** 62 }), "SEC-246: huge Duration milliseconds are refused");
});

__kitS.section("SEC-246: ordinary Duration components still construct", () => {
check(new Duration({ hours: 25, minutes: 1 }) instanceof Duration,
      "SEC-246: ordinary Duration components still construct");
if (fails === 0) print("test_time_data_w3_fixes: all " + n + " checks passed");
else print("test_time_data_w3_fixes: " + fails + " FAILED of " + n);
});
__kitS.end();
