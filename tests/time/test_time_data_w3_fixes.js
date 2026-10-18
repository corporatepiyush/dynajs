// timeout: 180
// Split from tests/test_data_w3_fixes.js: the time sections.
import { TOML } from "dyna:config";
import { RRule, Duration, date, parseDurationMs, parseDurationSecs } from "dyna:time";
import { setNativeMemoryLimit } from "dyna:sys";

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
await throws(() => new Duration({ years: 2 ** 62 }), "SEC-246: a huge Duration year is refused");
await throws(() => new Duration({ milliseconds: 2 ** 62 }), "SEC-246: huge Duration milliseconds are refused");
check(new Duration({ hours: 25, minutes: 1 }) instanceof Duration,
      "SEC-246: ordinary Duration components still construct");
await throws(() => date(2024, 1, 2 ** 62), "SEC-247: date() refuses a huge day field");
await throws(() => date(2024, 1, 1, 2 ** 40), "SEC-247: date() refuses a huge hour field");
eq(date(1970, 1, 1), 0, "SEC-247: the epoch still converts");
{
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
}
eq(parseDurationMs("2784249659845191438ns"), 2784249659845.1914,
   "COMPAT-262: parseDurationMs is correctly rounded (was 1 ulp high)");
eq(parseDurationMs("-8531699129511414324ns"), -8531699129511.414,
   "COMPAT-262: negative ns is correctly rounded");
eq(parseDurationMs("8785356068037452444ns"), 8785356068037.452,
   "COMPAT-262: a third counterexample");
eq(parseDurationSecs("1577960769301841103ns"), 1577960769.301841,
   "COMPAT-262: parseDurationSecs is correctly rounded");
eq(parseDurationMs("200000h"), 7.2e11, "COMPAT-262: the documented exact case still holds");
eq(parseDurationSecs("4581490823437043614ns"), 4581490823.437043,
   "COMPAT-262: seconds counterexample");
{
    const pos = [];
    for (let i = 0; i < 800; i++) pos.push(1 + (i % 336));
    const days = [], months = [];
    for (let d = 1; d <= 28; d++) days.push(d);
    for (let m = 1; m <= 12; m++) months.push(m);
    const r = new RRule({ freq: "YEARLY", dtstart: date(2026, 1, 1),
        bymonth: months, bymonthday: days, bysetpos: pos });
    const t0 = Date.now();
    const out = r.between(date(2026, 1, 1), date(2126, 1, 1));
    const ms = Date.now() - t0;
    check(out.length > 0 && ms < 2000,
          "OPT-056: a 800-entry duplicated BYSETPOS rule between 2026..2126 in " + ms + " ms");
    let sortedUnique = true;
    for (let i = 1; i < out.length; i++)
        if (!(out[i] > out[i - 1])) { sortedUnique = false; break; }
    check(sortedUnique, "OPT-056: BYSETPOS results stay sorted and de-duplicated");
}
if (fails === 0) print("test_time_data_w3_fixes: all " + n + " checks passed");
else print("test_time_data_w3_fixes: " + fails + " FAILED of " + n);
