// covers: dyna:time.Format
// F-time-1: layouts knew only the 3-letter tokens, so "Monday" rendered as "Mon" + "day" ("Tueday") and "January"
// as "Jan" + "uary". Go's layout tokens include the long names (time.Format: "January" stdLongMonth, "Monday"
// stdLongWeekDay), matched before the short ones; dynajs now has them in formatUnix, Format.format and Format.parse.
// Oracles: Go's documented outputs for fixed instants (time.Unix(0,0).UTC().Format("Monday, January 02 2006") is
// "Thursday, January 01 1970"), and JS Date's getUTCDay/getUTCMonth over a range of days with English name tables.
// The short tokens are unchanged ("Mon Jan" stays "Thu Jan").
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_time_format_long_names");
import { Format, formatUnix } from "dyna:time";

let n = 0, fails = 0;
function ok(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { ok(a === b, m + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")"); }
const WD = ["Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"];
const MO = ["January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"];

__kitS.section("Go reference outputs", () => {
    eq(formatUnix(0, "Monday, January 02 2006"), "Thursday, January 01 1970", "unix 0 (Go doc example layout)");
    eq(formatUnix(0, "Mon Jan 02"), "Thu Jan 01", "short tokens unchanged");
    eq(formatUnix(0, "Monday Mon January Jan"), "Thursday Thu January Jan", "long and short side by side");
    eq(formatUnix(1136239445, "Monday, 02-January-2006 15:04:05"), "Monday, 02-January-2006 22:04:05", "Go's reference time 2006-01-02T15:04:05-07:00 in UTC");
    eq(formatUnix(0, "Mondays Janu"), "Thursdays Janu", "a long token followed by letters, a short token followed by u (Go: stdMonth + literal)");
    eq(new Format("Monday January 02").format(86400 * 40), "Tuesday February 10", "Format.format");
    eq(formatUnix(0, "Monday", { offsetMinutes: -60 }), "Wednesday", "offset moves the weekday");
});

__kitS.section("every weekday and month name against JS Date", () => {
    const f = new Format("Monday|January|Mon|Jan|02|2006");
    for (let d = -400; d < 800; d += 7) {
        const sec = d * 86400 + 3600 * (d & 15);
        const dt = new Date(sec * 1000);
        const want = WD[dt.getUTCDay()] + "|" + MO[dt.getUTCMonth()] + "|" + WD[dt.getUTCDay()].slice(0, 3) + "|" + MO[dt.getUTCMonth()].slice(0, 3) + "|" +
            String(dt.getUTCDate()).padStart(2, "0") + "|" + dt.getUTCFullYear();
        eq(f.format(sec), want, "day " + d);
        eq(formatUnix(sec, "Monday|January|Mon|Jan|02|2006"), want, "formatUnix day " + d);
    }
});

__kitS.section("Format.parse reads the long names back", () => {
    const f = new Format("Monday January 02 2006");
    eq(f.parse("Tuesday February 10 1970"), 86400 * 40, "parse long names");
    for (let m = 0; m < 12; m++) {
        const sec = Date.UTC(2024, m, 15) / 1000;
        eq(f.parse(f.format(sec)), sec, "round trip " + MO[m]);
    }
    let threw = false;
    try { f.parse("Tue February 10 1970"); } catch (e) { threw = e instanceof SyntaxError; }
    ok(threw, "a short name where the layout has the long one is a SyntaxError");
});

if (fails) throw new Error(fails + " of " + n + " checks failed");
print("test_time_format_long_names: " + n + " checks passed");
__kitS.end();
