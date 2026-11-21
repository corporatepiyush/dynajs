// covers: dyna:time.Format
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_time_format_offset");
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

__kitS.section("%z at the zero offset is Z", () => {
    const L = "2006-01-02T15:04:05%z";
    const f0 = new Format(L);
    eq(f0.format(0), "1970-01-01T00:00:00Z", "%z at the zero offset is Z");
    const f = new Format(L, { offsetMinutes: 330 });
    eq(f.format(0), "1970-01-01T05:30:00+05:30", "%z emits the configured offset");
    eq(f.parse("1970-01-01T05:30:00+05:30"), 0, "%z parse round-trips");
    eq(f0.parse("1970-01-01T00:00:00Z"), 0, "Z parses");
    eq(new Format(L, { offsetMinutes: -300 }).format(0),
       "1969-12-31T19:00:00-05:00", "%z with a negative offset");
    eq(f.parse("1970-01-01T00:00:00Z"), 0, "parsed offset beats config");
    const g = new Format("2006-01-02 15:04:05", { offsetMinutes: 60 });
    eq(g.parse(g.format(0)), 0, "offset-only format/parse round-trips");
    eq(g.parse("1970-01-01 01:00:00"), 0, "fields read at the configured offset");
    throws(() => f.parse("1970-01-01T05:30:00+0530"), SyntaxError, /./,
           "%z without the colon refuses");
    throws(() => f.parse("1970-01-01T05:30:00"), SyntaxError, /./,
           "missing offset refuses");
    throws(() => f.parse("1970-01-01T05:30:00+24:00"), SyntaxError, /./,
           "RFC 3339 offset bound enforced in parse");
    throws(() => new Format("2006%z %z").parse("1970+01:00 +02:00"), SyntaxError, /./,
           "two %z tokens that disagree refuse");
    eq(new Format("2006%z %z", { offsetMinutes: 60 }).parse("1970+01:00 +01:00"),
       -3600, "two %z tokens that agree are accepted");
});

__kitS.section("a throwing offsetMinutes getter propagates", () => {
    let trapped = false;
    try { formatRFC3339(0, { get offsetMinutes() { throw new Error("trap"); } }); }
    catch (e) { trapped = String(e.message) === "trap"; }
    ok(trapped, "a throwing offsetMinutes getter propagates");
    throws(() => formatRFC3339(0, { offsetMinutes: NaN }), RangeError, /integer/,
           "NaN offset refuses");
    throws(() => formatRFC3339(0, { offsetMinutes: Infinity }), RangeError, /integer|1439/,
           "Infinity refuses");
    eq(formatRFC3339(0, { offsetMinutes: -0 }), "1970-01-01T00:00:00Z", "-0 is the zero offset");
    throws(() => formatRFC3339(0, [1]), TypeError, /unknown option "0"/,
           "an array bag refuses");
    eq(formatUnix(0, "%z", null), "Z", "null options argument is no-options");
    throws(() => formatUnix(0, "%z", { offsetMinutes: null }), TypeError,
           /must be a number/, "offsetMinutes: null is not a number");
    eq(formatUnix(0, "%z", undefined), "Z", "undefined bag is no-options");
    eq(new Format("%zz", { offsetMinutes: 60 }).format(0), "+01:00z",
       "%zz is the token plus a literal z");
    eq(new Format("%%%z", { offsetMinutes: 60 }).format(0), "%%+01:00",
       "two literal percents then the token");
    eq(new Format("%z", { offsetMinutes: 60 }).parse("+23:59"), -86340,
       "+23:59 parses (RFC 3339 bound)");
    eq(new Format("%z").parse("-00:00"), 0, "-00:00 is the zero offset");
    eq(new Format("%z").parse("z"), 0, "lowercase z parses");
    throws(() => new Format("%z").parse("+0:00"), SyntaxError, /./, "1-digit hour refuses");
    throws(() => new Format("%z").parse("+00:0"), SyntaxError, /./, "1-digit minute refuses");
    throws(() => new Format("%z").parse("Z "), SyntaxError, /./, "trailing input refuses");
    eq(formatRFC3339(0, { offsetMinutes: 1439 }), "1970-01-01T23:59:00+23:59",
       "+23:59 emits");
    eq(formatRFC3339(0, { offsetMinutes: -1439 }), "1969-12-31T00:01:00-23:59",
       "-23:59 emits");
    eq(parseRFC3339("1970-01-01T00:00:00+23:59").sec, -86340,
       "parseRFC3339 accepts the same bound the format side emits");
});
__kitS.end();

print("test_time_offset: " + n + " assertions, " + bad + " failures");
if (bad) throw new Error(bad + " failures");
