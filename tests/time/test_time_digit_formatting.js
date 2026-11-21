// covers: c:src/modules/time/time-temporal.c
// The digit writer every time formatter shares (dyn_time_utoa_pad) emits two digits per step from a
// 200-byte pair table, right to left, and PlainDate / PlainTime / PlainDateTime toString build their
// text with it instead of snprintf (vfprintf was 15% of PlainDate#toString; 26% faster without it).
// Years outside 0..9999 and negative clock values keep the snprintf path. Each case below is checked
// against an independent JS formatter built on padStart and Date, over sweeps that reach every pair
// in the table, every pad width (0, 2, 3, 4, 9) and both sides of each fallback boundary.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_time_digit_formatting");
import * as T from "dyna:time";

let pass = 0, fail = 0;
const eq = (a, b, m) => { if (a === b) pass++; else { fail++; print("FAIL " + m + ": got " + a + ", want " + b); } };
const p = (v, w) => String(v).padStart(w, "0");
const isoDate = (y, m, d) => (y < 0 || y > 9999 ? (y < 0 ? "-" : "+") + p(Math.abs(y), 6) : p(y, 4)) + "-" + p(m, 2) + "-" + p(d, 2);
const isoClock = (h, mi, s, ms) => p(h, 2) + ":" + p(mi, 2) + ":" + p(s, 2) + (ms ? "." + p(ms, 3) : "");
const dim = (y, m) => new Date(Date.UTC(2000, m, 0)).getUTCDate() - (m === 2 && !((y % 4 === 0 && y % 100 !== 0) || y % 400 === 0) ? 1 : 0);

__kitS.section("PlainDate toString across the 4-digit range and both fallbacks", () => {
    const years = [0, 1, 9, 10, 99, 100, 999, 1000, 1969, 1970, 2000, 2024, 9998, 9999, 10000, 10001, 99999, 123456, -1, -9, -10, -999, -1000, -9999, -10000, -123456];
    for (let y = 0; y < 10000; y += 37) years.push(y);
    for (const y of years)
        for (const [m, d] of [[1, 1], [2, dim(y, 2)], [7, 4], [9, 30], [10, 10], [12, 31]])
            eq(new T.PlainDate(y, m, d).toString(), isoDate(y, m, d), "PlainDate " + y + "-" + m + "-" + d);
    for (let m = 1; m <= 12; m++)
        for (let d = 1; d <= dim(2023, m); d++)
            eq(new T.PlainDate(2023, m, d).toString(), isoDate(2023, m, d), "every day of 2023 " + m + "/" + d);
});

__kitS.section("PlainTime toString: every hour, minute, second and millisecond digit pair", () => {
    for (let h = 0; h < 24; h++)
        for (let mi = 0; mi < 60; mi += 7)
            eq(new T.PlainTime(h, mi, (h * 7 + mi) % 60, 0).toString(), isoClock(h, mi, (h * 7 + mi) % 60, 0), "clock " + h + ":" + mi);
    for (let ms = 0; ms < 1000; ms++)
        eq(new T.PlainTime(12, 34, 56, ms).toString(), isoClock(12, 34, 56, ms), "millisecond " + ms);
    for (let s = 0; s < 60; s++)
        eq(new T.PlainTime(0, 0, s).toString(), isoClock(0, 0, s, 0), "second " + s);
    eq(new T.PlainTime(23, 59, 59, 999).toString(), "23:59:59.999", "last millisecond of the day");
    eq(new T.PlainTime().toString(), "00:00:00", "midnight");
});

__kitS.section("PlainDateTime toString joins both halves", () => {
    for (const y of [0, 7, 99, 2024, 9999, 10000, -1, -5])
        for (const [h, mi, s, ms] of [[0, 0, 0, 0], [4, 5, 6, 70], [23, 59, 59, 999], [12, 0, 0, 1], [9, 9, 9, 100]])
            eq(new T.PlainDateTime(y, 2, 3, h, mi, s, ms).toString(), isoDate(y, 2, 3) + "T" + isoClock(h, mi, s, ms), "PlainDateTime " + y + " " + h + ":" + mi + ":" + s + "." + ms);
    eq(T.toPlainDateTime(T.parseDate("2024-02-29"), T.parseTime("07:08:09.010")).toString(), "2024-02-29T07:08:09.010", "parsed halves");
    eq(new T.PlainDateTime(2024, 1, 1, 23, 59, 59, 999).add(new T.Duration({ milliseconds: 1 })).toString(), "2024-01-02T00:00:00", "carry into the date");
    eq(T.parseDate("2020-01-01").toString(), "2020-01-01", "parseDate round trip");
});

__kitS.section("RFC 3339 and layouts against Date", () => {
    for (let i = 0; i < 4000; i++) {
        const sec = Math.floor(-62135596800 + i * 63113851.3 + (i % 97) * 3607);
        const nsec = (i * 2654435761) % 1e9;
        const base = new Date(sec * 1000).toISOString().slice(0, 19);
        const frac = nsec ? "." + p(nsec, 9).replace(/0+$/, "") : "";
        eq(T.formatRFC3339(sec, nsec, true), base + frac + "Z", "formatRFC3339 " + sec + " " + nsec);
    }
    for (const ns of [0, 1, 9, 10, 99, 100, 120000000, 999999999, 100000000, 5])
        eq(T.formatRFC3339(0, ns, true), "1970-01-01T00:00:00" + (ns ? "." + p(ns, 9).replace(/0+$/, "") : "") + "Z", "nsec pad " + ns);
    const mon = ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"], wd = ["Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"];
    for (const off of [0, 330, -480, 59, -1, 845, -1439, 1439]) {
        const f = new T.Format("02/01/2006 15h04m05s Jan Mon %z", { offsetMinutes: off });
        const z = off === 0 ? "Z" : (off < 0 ? "-" : "+") + p(Math.floor(Math.abs(off) / 60), 2) + ":" + p(Math.abs(off) % 60, 2);
        for (let i = 0; i < 600; i++) {
            const sec = -62135596800 + 3600 * 24 + Math.floor(i * 526142737.7) + i * 3607;
            const d = new Date((sec + off * 60) * 1000);
            const want = p(d.getUTCDate(), 2) + "/" + p(d.getUTCMonth() + 1, 2) + "/" + p(d.getUTCFullYear(), 4) + " " + p(d.getUTCHours(), 2) + "h" + p(d.getUTCMinutes(), 2) + "m" + p(d.getUTCSeconds(), 2) + "s " + mon[d.getUTCMonth()] + " " + wd[d.getUTCDay()] + " " + z;
            eq(f.format(sec), want, "layout " + sec + " at " + off);
        }
    }
    eq(new T.Format("2006-01-02T15:04:05.000000000").format(1700000000), "2023-11-14T22:13:20.000000000", "nine zero fraction digits");
});

__kitS.section("durations print their digits", () => {
    for (let i = 0; i < 2000; i++) {
        const h = i % 50, m = i % 60, s = 1 + (i * 7) % 59, ms = i % 1000;
        const want = (h ? h + "h" : "") + (h || m ? m + "m" : "") + s + (ms ? "." + p(ms, 3).replace(/0+$/, "") : "") + "s";
        eq(T.durationString(T.parseDuration(h + "h" + m + "m" + s + "." + p(ms, 3) + "s")), want, "duration " + h + "h" + m + "m" + s + "." + ms);
    }
    for (const [txt, want] of [["0.6s", "600ms"], ["1ms", "1ms"], ["1.5us", "1.5\u00b5s"], ["1ns", "1ns"], ["0s", "0s"], ["59m59.999s", "59m59.999s"], ["1000h", "1000h0m0s"]])
        eq(T.durationString(T.parseDuration(txt)), want, "sub-second and wide units " + txt);
    eq(T.durationString(T.parseDuration("1h0m0.000000001s")), "1h0m0.000000001s", "one nanosecond");
    eq(T.durationString(T.parseDuration("100h")), "100h0m0s", "three-digit hours");
});

__kitS.end();
print(`test_time_digit_formatting: ${pass} passed, ${fail} failed`);
if (fail) throw new Error(fail + " failed");
