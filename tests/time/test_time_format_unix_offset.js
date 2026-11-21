// covers: dyna:time.formatUnix
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_time_format_unix_offset");
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

__kitS.section("2006-01-02 15:04:05 %z Mon Jan", () => {
    const layouts = ["%z", "2006-01-02T15:04:05%z", "%z%z", "2006%z01",
                     "2006-01-02 15:04:05 %z Mon Jan", "%z2006", "15:%z04"];
    const times = [0, -1, 1234567890, -2208988800, 32503680000];
    const offs = [0, 330, -330, 1439, -1439, 60, -1];
    for (const L of layouts) {
        for (const t of times) {
            for (const m of offs) {
                const fu = formatUnix(t, L, { offsetMinutes: m });
                const ff = new Format(L, { offsetMinutes: m }).format(t);
                eq(fu, ff, "Format/formatUnix byte-identity L=" + L +
                    " t=" + t + " m=" + m);
            }
        }
    }
    eq(formatUnix(0, "2006%z", { offsetMinutes: 330 }), "1970+05:30",
       "%z alone at +05:30");
    eq(formatUnix(0, "%z", {}), "Z", "%z alone at zero");
    throws(() => formatUnix(0, "%z", { offset: 1 }), TypeError,
           /unknown option "offset" \(valid: offsetMinutes\)/,
           "formatUnix bag is strict");
    throws(() => new Format("%z", { off: 1 }), TypeError,
           /unknown option "off" \(valid: offsetMinutes\)/,
           "Format bag is strict");
    throws(() => formatUnix(0, "%z", { offsetMinutes: "x" }), TypeError,
           /must be a number/, "non-numeric offset refuses (typed, not coerced)");
});

__kitS.section("two percent signs are literal", () => {
    eq(formatUnix(0, "%%"), "%%", "two percent signs are literal");
    eq(formatUnix(0, "a%zb", { offsetMinutes: 60 }), "a+01:00b",
       "%z embedded in literals");
    eq(new Format("z%").format(0), "z%", "trailing percent is literal");
});
__kitS.end();

{
    throws(() => formatRFC3339(0, { offsetMinutes: 0 }, true), TypeError,
           /exactly 2 arguments/, "the bag form refuses a third argument");
    throws(() => formatRFC3339(0, { nsec: 1 }, undefined), TypeError,
           /exactly 2 arguments/, "even an explicit undefined third");
    eq(formatRFC3339(0, 0, true), "1970-01-01T00:00:00Z",
       "the legacy (nsec, utc) form is unchanged");
    eq(formatRFC3339(0, 500000000, true), "1970-01-01T00:00:00.5Z",
       "the legacy (nsec, utc) form with a fraction is unchanged");
    throws(() => formatUnix(0, "%z", "x"), TypeError, /options must be an object/,
           "formatUnix refuses a string bag");
    throws(() => formatUnix(0, "%z", 3), TypeError, /options must be an object/,
           "formatUnix refuses a number bag");
    throws(() => new Format("%z", "x"), TypeError, /options must be an object/,
           "Format refuses a string bag");
    eq(formatUnix(0, "%z", null), "Z", "null opts is the no-options reading");
    eq(new Format("%z", null).format(0), "Z", "null opts on Format too");
}

print("test_time_offset: " + n + " assertions, " + bad + " failures");
if (bad) throw new Error(bad + " failures");
