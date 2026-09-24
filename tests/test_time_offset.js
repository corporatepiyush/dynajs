// flags: --std
/* test_time_offset.js --: offset-aware format side of dyna:time.
 *
 *   formatRFC3339(ts, { offsetMinutes, nsec })   fixed-offset rendering
 *   %z in Format/formatUnix layouts             the offset token
 *   new Format(layout, { offsetMinutes }) /
 *   formatUnix(ts, layout, { offsetMinutes })   the offset configuration
 *
 * Oracle discipline: every EXPECTED STRING is computed by the engine's own
 * Date.toISOString (the ECMAScript core, an independent implementation of
 * civil-time decomposition -- not dyna:time's dyn_civil_from_days), then the
 * RFC 3339 numeric-offset suffix (section 5.6: "+HH:MM"/"-HH:MM", "Z" at
 * zero) is attached by hand. The Format.format vs formatUnix byte-identity
 * holds over %z too (the same differential tests/test_time_format.js keeps
 * for the other tokens). The parse round-trip is a property of the pair.
 */
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

/* The reference: unix seconds shifted to `m` minutes east of UTC, rendered
 * by Date.toISOString and re-suffixed per RFC 3339. */
function rfc3339ref(t, m) {
    const d = new Date((t + m * 60) * 1000);
    const base = d.toISOString().replace(/\.\d+Z$/, "");
    if (m === 0) return base + "Z";
    const sign = m < 0 ? "-" : "+";
    const a = Math.abs(m);
    return base + sign + String(Math.floor(a / 60)).padStart(2, "0") + ":" +
           String(a % 60).padStart(2, "0");
}

/* ---- 1. the legacy positional form is untouched ---- */
eq(formatRFC3339(0), "1970-01-01T00:00:00Z", "legacy zero");
eq(formatRFC3339(0, 500000000), "1970-01-01T00:00:00.5Z",
   "legacy nsec (trailing fraction zeros are trimmed, as ever)");
eq(formatRFC3339(date(2026, 8, 17, 10, 30, 0)), "2026-08-17T10:30:00Z", "legacy date");

/* ---- 2. the bag form, against the Date reference ---- */
eq(formatRFC3339(0, {}), "1970-01-01T00:00:00Z", "empty bag == UTC");
eq(formatRFC3339(0, { offsetMinutes: 0 }), "1970-01-01T00:00:00Z", "zero offset is Z");
eq(formatRFC3339(0, { offsetMinutes: 330 }), "1970-01-01T05:30:00+05:30", "+05:30");
eq(formatRFC3339(0, { offsetMinutes: -300 }), "1969-12-31T19:00:00-05:00", "-05:00");
eq(formatRFC3339(0, { offsetMinutes: 60, nsec: 500000000 }),
   "1970-01-01T01:00:00.5+01:00", "offset + nsec");

{
    /* sweep x differential vs Date: negative times, month/year edges, the
     * offset extremes, and offsets that shift the date across midnight. */
    const times = [0, 1, -1, 86399, 86400, 1234567890, 1583020800,
                   1767225600, -2208988800, 32503680000];
    const offs = [0, 1, -1, 60, -60, 330, -330, 345, 840, -720, 1439, -1439,
                  59, -59];
    for (const t of times) {
        for (const m of offs) {
            eq(formatRFC3339(t, { offsetMinutes: m }), rfc3339ref(t, m),
               "Date differential t=" + t + " m=" + m);
            /* property: the pair round-trips */
            eq(parseRFC3339(formatRFC3339(t, { offsetMinutes: m })).sec, t,
               "round-trip t=" + t + " m=" + m);
        }
    }
}

/* ---- 3.: the bag is strict ---- */
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

/* ---- 4. %z in Format layouts ---- */
{
    const L = "2006-01-02T15:04:05%z";
    const f0 = new Format(L);
    eq(f0.format(0), "1970-01-01T00:00:00Z", "%z at the zero offset is Z");
    const f = new Format(L, { offsetMinutes: 330 });
    eq(f.format(0), "1970-01-01T05:30:00+05:30", "%z emits the configured offset");
    eq(f.parse("1970-01-01T05:30:00+05:30"), 0, "%z parse round-trips");
    eq(f0.parse("1970-01-01T00:00:00Z"), 0, "Z parses");
    eq(new Format(L, { offsetMinutes: -300 }).format(0),
       "1969-12-31T19:00:00-05:00", "%z with a negative offset");
    /* the parsed offset OVERRIDES the configured one: the string is the
       truth when it carries the zone (with the config's 19800s wrongly
       subtracted this would be -19800) */
    eq(f.parse("1970-01-01T00:00:00Z"), 0, "parsed offset beats config");
    /* without %z the configured offset is what the fields are read at */
    const g = new Format("2006-01-02 15:04:05", { offsetMinutes: 60 });
    eq(g.parse(g.format(0)), 0, "offset-only format/parse round-trips");
    eq(g.parse("1970-01-01 01:00:00"), 0, "fields read at the configured offset");
    /* shape refusals */
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
}

/* ---- 5. %z through formatUnix + the Format/formatUnix differential ---- */
{
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
    /* the two forms of %z emission agree with the RFC 3339 suffix */
    eq(formatUnix(0, "2006%z", { offsetMinutes: 330 }), "1970+05:30",
       "%z alone at +05:30");
    eq(formatUnix(0, "%z", {}), "Z", "%z alone at zero");
    /* strict bags on the two new carriers */
    throws(() => formatUnix(0, "%z", { offset: 1 }), TypeError,
           /unknown option "offset" \(valid: offsetMinutes\)/,
           "formatUnix bag is strict");
    throws(() => new Format("%z", { off: 1 }), TypeError,
           /unknown option "off" \(valid: offsetMinutes\)/,
           "Format bag is strict");
    throws(() => formatUnix(0, "%z", { offsetMinutes: "x" }), TypeError,
           /must be a number/, "non-numeric offset refuses (typed, not coerced)");
}

/* ---- 6. %z token lookalikes stay literals ---- */
{
    /* "%%z" is literal "%z"? No -- there is no escape: the tokenizer matches
       "%z" wherever it appears, and a bare "%" is a literal. Pin the split. */
    eq(formatUnix(0, "%%"), "%%", "two percent signs are literal");
    eq(formatUnix(0, "a%zb", { offsetMinutes: 60 }), "a+01:00b",
       "%z embedded in literals");
    eq(new Format("z%").format(0), "z%", "trailing percent is literal");
}

/* ---- 7. adversarial shapes (probe-derived; see scratch/e5_probe1.js) ---- */
{
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
    /* `null` is no options when it IS the options argument; as a bag
       PROPERTY VALUE it is not a number and the shared reader refuses it
       (the same policy formatRFC3339's offsetMinutes follows). */
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
}

/* ---- argument shape: the bag form refuses what it cannot mean ---- */
{
    /* the bag form is exactly (sec, opts): a third positional is refused,
       while the legacy (nsec, utc) tail keeps working */
    throws(() => formatRFC3339(0, { offsetMinutes: 0 }, true), TypeError,
           /exactly 2 arguments/, "the bag form refuses a third argument");
    throws(() => formatRFC3339(0, { nsec: 1 }, undefined), TypeError,
           /exactly 2 arguments/, "even an explicit undefined third");
    eq(formatRFC3339(0, 0, true), "1970-01-01T00:00:00Z",
       "the legacy (nsec, utc) form is unchanged");
    eq(formatRFC3339(0, 500000000, true), "1970-01-01T00:00:00.5Z",
       "the legacy (nsec, utc) form with a fraction is unchanged");
    /* a bag is an object or absent: a non-object refuses, it does not
       silently default */
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
