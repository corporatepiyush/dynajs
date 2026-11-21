// covers: dyna:time.formatRFC3339
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_time_format_rfc3339_offset");
import { formatRFC3339, formatUnix, parseRFC3339, Format, date } from "dyna:time";

let n = 0, bad = 0;
function ok(c, what) { n++; if (!c) { bad++; print("FAIL: " + what); } }
function eq(a, b, what) {
    ok(a === b, what + " (got " + a + ", want " + b + ")");
}
function throws(fn, ErrType, re, what) {
    let t = null;
    try { fn(); } catch (e) { t = e; }
    ok(t !== null, "expected throw: " + what);
    if (t === null) return;
    ok(t instanceof ErrType,
       what + " (wrong error type: " + (t && t.constructor && t.constructor.name) + ")");
    ok(!re || re.test(String(t.message)),
       what + " (message [" + String(t && t.message) + "] !~ " + re + ")");
}

function rfc3339ref(t, m) {
    const d = new Date((t + m * 60) * 1000);
    const base = d.toISOString().replace(/\.\d+Z$/, "");
    if (m === 0) return base + "Z";
    const sign = m < 0 ? "-" : "+";
    const a = Math.abs(m);
    return base + sign + String(Math.floor(a / 60)).padStart(2, "0") + ":" +
           String(a % 60).padStart(2, "0");
}

eq(formatRFC3339(0), "1970-01-01T00:00:00Z", "legacy zero");

eq(formatRFC3339(0, 500000000), "1970-01-01T00:00:00.5Z",
   "legacy nsec (trailing fraction zeros are trimmed, as ever)");

eq(formatRFC3339(0, {}), "1970-01-01T00:00:00Z", "empty bag == UTC");

eq(formatRFC3339(0, { offsetMinutes: 0 }), "1970-01-01T00:00:00Z", "zero offset is Z");

eq(formatRFC3339(0, { offsetMinutes: 330 }), "1970-01-01T05:30:00+05:30", "+05:30");

eq(formatRFC3339(0, { offsetMinutes: -300 }), "1969-12-31T19:00:00-05:00", "-05:00");

eq(formatRFC3339(0, { offsetMinutes: 60, nsec: 500000000 }),
   "1970-01-01T01:00:00.5+01:00", "offset + nsec");

__kitS.section("Date differential t", () => {
    const times = [0, 1, -1, 86399, 86400, 1234567890, 1583020800,
                   1767225600, -2208988800, 32503680000];
    const offs = [0, 1, -1, 60, -60, 330, -330, 345, 840, -720, 1439, -1439,
                  59, -59];
    for (const t of times) {
        for (const m of offs) {
            eq(formatRFC3339(t, { offsetMinutes: m }), rfc3339ref(t, m),
               "Date differential t=" + t + " m=" + m);
            eq(parseRFC3339(formatRFC3339(t, { offsetMinutes: m })).sec, t,
               "round-trip t=" + t + " m=" + m);
        }
    }
});

throws(() => formatRFC3339(0, { offsetminutes: 0 }), TypeError,
       /unknown option "offsetminutes" \(valid: nsec, offsetMinutes\)/,
       "misspelled key names the key and the valid set");

throws(() => formatRFC3339(0, { nsec: 1, offsetMinutes: 2, extra: 3 }), TypeError,
       /unknown option "extra"/, "extra key refuses");

throws(() => formatRFC3339(0, { offsetMinutes: 1.5 }), RangeError,
       /integer/, "fractional offset refuses");

throws(() => formatRFC3339(0, { offsetMinutes: 1440 }), RangeError,
       /\[-1439, 1439\]/, "+1440 minutes is past RFC 3339's +/-23:59");

throws(() => formatRFC3339(0, { offsetMinutes: -1440 }), RangeError,
       /\[-1439, 1439\]/, "-1440 minutes refuses");

eq(formatRFC3339(0, { offsetMinutes: 1439 }), rfc3339ref(0, 1439), "+1439 boundary");

eq(formatRFC3339(0, { offsetMinutes: -1439 }), rfc3339ref(0, -1439), "-1439 boundary");

throws(() => formatRFC3339(0, { nsec: 1e9 }), RangeError,
       /nsec must be in/, "nsec bound still enforced in the bag");

__kitS.section("refuses out-of-range input", () => {
    /* A second count the C library's localtime_r cannot convert must not
       read an uninitialised struct tm for the offset: the output has to be
       the UTC rendering, every time, on every stack. */
    for (const huge of [1e18, -1e18, 9.3e18, -9.3e18, 6.7e16, 1e15]) {
        let s2 = null, threw = null;
        try { s2 = formatRFC3339(huge); }
        catch (e) { threw = e; }
        if (threw) {
            ok(threw instanceof RangeError || threw instanceof TypeError,
               "formatRFC3339(" + huge + ") refuses out-of-range input (" +
               threw.message.slice(0, 40) + ")");
            continue;
        }
        ok(typeof s2 === "string" && s2.length > 0,
           "formatRFC3339(" + huge + ") renders (" + s2.slice(0, 24) + ")");
        ok(/Z$|[+-]\d\d:\d\d$/.test(s2),
           "formatRFC3339(" + huge + ") ends in an offset (" + s2.slice(-6) + ")");
        eq(formatRFC3339(huge), s2,
           "formatRFC3339(" + huge + ") is deterministic across calls");
    }
    eq(formatRFC3339(0, 0, true), "1970-01-01T00:00:00Z",
       "the explicit-UTC form is unchanged");
});
__kitS.end();

print("test_time_offset: " + n + " assertions, " + bad + " failures");
if (bad) throw new Error(bad + " failures");
