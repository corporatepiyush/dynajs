// timeout: 180
// covers: dyna:time.date
// Split from tests/test_data_w3_fixes.js: the time sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_time_date_data_w3_fixes");
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

await throws(() => date(2024, 1, 2 ** 62), "SEC-247: date() refuses a huge day field");

await throws(() => date(2024, 1, 1, 2 ** 40), "SEC-247: date() refuses a huge hour field");

eq(date(1970, 1, 1), 0, "SEC-247: the epoch still converts");

__kitS.section("OPT-056: a 800-entry duplicated BYSETPOS rule between 2026..2126 in", () => {
    const pos = [];
    for (let i = 0; i < 800; i++) pos.push(1 + (i % 336));
    const days = [], months = [];
    for (let d = 1; d <= 28; d++) days.push(d);
    for (let m = 1; m <= 12; m++) months.push(m);
    const r = new RRule({ freq: "YEARLY", dtstart: date(2026, 1, 1),
        bymonth: months, bymonthday: days, bysetpos: pos });
    const t0 = cpuNow();
    const out = r.between(date(2026, 1, 1), date(2126, 1, 1));
    const ms = cpuNow() - t0;
    check(out.length > 0 && ms < 2000,
          "OPT-056: a 800-entry duplicated BYSETPOS rule between 2026..2126 in " + ms + " ms");
    let sortedUnique = true;
    for (let i = 1; i < out.length; i++)
        if (!(out[i] > out[i - 1])) { sortedUnique = false; break; }
    check(sortedUnique, "OPT-056: BYSETPOS results stay sorted and de-duplicated");
});
__kitS.end();
if (fails === 0) print("test_time_data_w3_fixes: all " + n + " checks passed");
else print("test_time_data_w3_fixes: " + fails + " FAILED of " + n);
