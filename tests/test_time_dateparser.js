/* test_time_dateparser.js -- class DateParser (STDLIB_OOP_PLAN section 13).
 *
 * The item was "Date.create(string) -- natural-language date parser plus
 * per-locale format masks", restated as a compiled capability: the locale is
 * the configuration, and one instance parses unbounded strings.
 *
 * THE REASON A LOCALE EXISTS HERE, and it is the test that matters most:
 * "03/04/2026" is 4 March in en-GB, fr, de and es, and 3 April in en-US. Both
 * readings are well-formed, so a parser that guessed would be silently wrong
 * for one of them on input that looks perfect. Section 2 pins both.
 *
 * Every relative case is parsed against an EXPLICIT `now`, so "tomorrow" is a
 * fixed answer rather than a race with the clock. A test whose expected value
 * depends on when it runs is not a test.
 */
import { DateParser, formatRFC3339, parseRFC3339 } from "dyna:time";

let n = 0;
function assert(c, m) { n++; if (!c) throw new Error("assertion failed: " + m); }
function eq(got, want, m) {
    n++;
    if (got !== want)
        throw new Error("assertion failed: " + m + "\n  got:  " + got + "\n  want: " + want);
}
function throws(fn, kind, m) {
    n++;
    try { fn(); } catch (e) {
        if (kind && !(e instanceof kind)) throw new Error((m || "wrong error") + ": " + e);
        return String(e.message || e);
    }
    throw new Error((m || "expected a throw") + " but none happened");
}

/* Tuesday 28 July 2026, 15:30:00 UTC. The weekday is asserted rather than
 * assumed, because every "next monday" answer below depends on it. */
const NOW = Date.UTC(2026, 6, 28, 15, 30, 0) / 1000;
eq(new Date(NOW * 1000).getUTCDay(), 2, "the fixed now is a Tuesday");

const us = new DateParser("en-US", { now: NOW });
const gb = new DateParser("en-GB", { now: NOW });
const fr = new DateParser("fr", { now: NOW });
const de = new DateParser("de", { now: NOW });
const es = new DateParser("es", { now: NOW });

const iso = t => (t === null ? null : formatRFC3339(t));

/* ==================================================================== *
 *  1. Absolute forms
 * ==================================================================== */
{
    const cases = [
        ["2026-07-28", "2026-07-28T00:00:00Z"],
        ["2026/07/28", "2026-07-28T00:00:00Z"],
        ["2026-07-28 14:30", "2026-07-28T14:30:00Z"],
        ["2026-07-28T14:30:05", "2026-07-28T14:30:05Z"],
        ["2026-07-28 2:30 pm", "2026-07-28T14:30:00Z"],
        ["2026-07-28 12:30 am", "2026-07-28T00:30:00Z"],
        ["2026-07-28 12:30 pm", "2026-07-28T12:30:00Z"],
        ["July 28, 2026", "2026-07-28T00:00:00Z"],
        ["july 28 2026", "2026-07-28T00:00:00Z"],
        ["JULY 28 2026", "2026-07-28T00:00:00Z"],
        ["Jul 28 2026", "2026-07-28T00:00:00Z"],
        ["28 July 2026", "2026-07-28T00:00:00Z"],
        ["28 Jul 2026", "2026-07-28T00:00:00Z"],
        ["Tuesday, 28 July 2026", "2026-07-28T00:00:00Z"],
        ["Tue 28 July 2026", "2026-07-28T00:00:00Z"],
        ["28 July 2026 9:05", "2026-07-28T09:05:00Z"],
        /* two-digit years use the POSIX window: 69 and below are 2000s */
        ["Jul 28 26", "2026-07-28T00:00:00Z"],
        ["Jul 28 69", "2069-07-28T00:00:00Z"],
        ["Jul 28 70", "1970-07-28T00:00:00Z"],
        ["Jul 28 99", "1999-07-28T00:00:00Z"],
    ];
    for (const [text, want] of cases)
        eq(iso(us.parse(text)), want, "parse " + JSON.stringify(text));

    /* Leading and trailing space, and separators, are noise. */
    eq(iso(us.parse("   2026-07-28   ")), "2026-07-28T00:00:00Z", "surrounding space");
}

/* ==================================================================== *
 *  2. THE LOCALE: the same digits, two different days
 * ==================================================================== */
{
    eq(iso(us.parse("03/04/2026")), "2026-03-04T00:00:00Z", "en-US reads month first");
    eq(iso(gb.parse("03/04/2026")), "2026-04-03T00:00:00Z", "en-GB reads day first");
    eq(iso(fr.parse("03/04/2026")), "2026-04-03T00:00:00Z", "fr reads day first");
    eq(iso(de.parse("03.04.2026")), "2026-04-03T00:00:00Z", "de, with dots");
    eq(iso(es.parse("03-04-2026")), "2026-04-03T00:00:00Z", "es, with dashes");
    assert(us.dayFirst === false && gb.dayFirst === true,
           "and the convention is readable off the instance");
    eq(us.locale, "en-US", "locale getter");

    /* A day above 12 is unambiguous, and en-US still reads it as month-first
     * and therefore REFUSES it -- rather than silently swapping the fields,
     * which would make the locale mean nothing. */
    eq(us.parse("28/07/2026"), null, "en-US refuses 28 as a month");
    eq(iso(gb.parse("28/07/2026")), "2026-07-28T00:00:00Z", "en-GB accepts it");

    /* Month names in each locale. */
    eq(iso(fr.parse("4 mars 2026")), "2026-03-04T00:00:00Z", "fr month name");
    eq(iso(fr.parse("mercredi 4 mars 2026")), "2026-03-04T00:00:00Z", "fr weekday");
    eq(iso(de.parse("4. Marz 2026")), "2026-03-04T00:00:00Z", "de month name");
    eq(iso(es.parse("4 de marzo 2026")), null, "es 'de' is not skipped, and says so");
    eq(iso(es.parse("4 marzo 2026")), "2026-03-04T00:00:00Z", "es month name");

    /* A month name from ANOTHER locale is not recognised -- the tables are the
     * configuration, so this is the fact that they are actually consulted. */
    eq(fr.parse("4 March 2026"), null, "fr does not know English months");
    eq(us.parse("4 mars 2026"), null, "en-US does not know French months");
}

/* ==================================================================== *
 *  3. Relative words, against a fixed now
 * ==================================================================== */
{
    const cases = [
        ["now", "2026-07-28T15:30:00Z"],
        ["today", "2026-07-28T00:00:00Z"],
        ["tomorrow", "2026-07-29T00:00:00Z"],
        ["yesterday", "2026-07-27T00:00:00Z"],
        ["in 3 days", "2026-07-31T15:30:00Z"],
        ["in 1 hour", "2026-07-28T16:30:00Z"],
        ["in 90 minutes", "2026-07-28T17:00:00Z"],
        ["2 weeks ago", "2026-07-14T15:30:00Z"],
        ["30 seconds ago", "2026-07-28T15:29:30Z"],
        ["in 2 months", "2026-09-28T15:30:00Z"],
        ["1 year ago", "2025-07-28T15:30:00Z"],
        /* from a Tuesday */
        ["next monday", "2026-08-03T00:00:00Z"],
        ["next tuesday", "2026-08-04T00:00:00Z"],
        ["last friday", "2026-07-24T00:00:00Z"],
        ["last tuesday", "2026-07-21T00:00:00Z"],
    ];
    for (const [text, want] of cases)
        eq(iso(us.parse(text)), want, "relative " + JSON.stringify(text));

    /* "next <today's weekday>" is a week away, not today -- the word means
     * the NEXT one, and zero would make it a no-op. */
    eq(iso(us.parse("next tuesday")), "2026-08-04T00:00:00Z", "next of today is +7");
    eq(iso(us.parse("last tuesday")), "2026-07-21T00:00:00Z", "last of today is -7");

    /* A bare count is not a date: "3 days" is a duration. */
    eq(us.parse("3 days"), null, "a bare duration is not a date");

    /* Calendar months clamp rather than overflow: 31 January plus one month is
     * 28 February, never 3 March. */
    const jan31 = new DateParser("en-US", { now: Date.UTC(2026, 0, 31) / 1000 });
    eq(iso(jan31.parse("in 1 month")), "2026-02-28T00:00:00Z", "month addition clamps");
    const feb29 = new DateParser("en-US", { now: Date.UTC(2024, 1, 29) / 1000 });
    eq(iso(feb29.parse("in 1 year")), "2025-02-28T00:00:00Z", "a leap day clamps");
}

/* ==================================================================== *
 *  4. What it refuses, and it refuses by returning null
 *
 *  null rather than a throw: this parses text a human typed, so "not a date I
 *  recognise" is an ordinary outcome. A caller who wants an error writes one
 *  where the failure means something.
 * ==================================================================== */
{
    for (const bad of ["", "   ", "nonsense", "the 32nd of Octember",
                       "2026-02-30", "2026-13-01", "2026-00-10", "2026-07-00",
                       "2026-07-28 25:00", "2026-07-28 12:61",
                       "3:45 pm 2026-07-28", "2026", "July", "28",
                       "2026-07-28 trailing", "in days", "ago"])
        eq(us.parse(bad), null, "refuses " + JSON.stringify(bad));

    /* 29 February is a date in a leap year and not in a common one. */
    eq(iso(us.parse("2024-02-29")), "2024-02-29T00:00:00Z", "leap day exists in 2024");
    eq(us.parse("2026-02-29"), null, "and not in 2026");
    eq(iso(us.parse("2000-02-29")), "2000-02-29T00:00:00Z", "2000 is a leap year");
    eq(us.parse("1900-02-29"), null, "1900 is not");
}

/* ==================================================================== *
 *  5. It agrees with the parser that already existed
 * ==================================================================== */
{
    for (const s of ["2026-07-28T14:30:05Z", "1970-01-01T00:00:00Z",
                     "1999-12-31T23:59:59Z"]) {
        const viaRfc = parseRFC3339(s).sec;
        const viaDp = us.parse(s.replace("Z", "").replace("T", " "));
        eq(viaDp, viaRfc, "DateParser agrees with parseRFC3339 on " + s);
    }
}

/* ==================================================================== *
 *  6. Construction and adversarial arguments
 * ==================================================================== */
{
    const msg = throws(() => new DateParser("xx"), RangeError, "an unknown locale");
    assert(msg.includes("en-US"), "and the error lists the known ones: " + msg);

    /* No locale at all is en-US, and the default is stated rather than implied. */
    eq(new DateParser().locale, "en-US", "the default locale");
    eq(new DateParser(undefined, { now: NOW }).parse("03/04/2026"),
       us.parse("03/04/2026"), "undefined selects the default too");

    /* The text is coerced with ToString, so toString is the hook that fires --
     * a valueOf here would exercise nothing (CLAUDE.md section 8). */
    let ranToString = 0, ranValueOf = 0;
    const probe = {
        toString() { ranToString++; return "2026-07-28"; },
        valueOf() { ranValueOf++; return 1; },
    };
    eq(iso(us.parse(probe)), "2026-07-28T00:00:00Z", "a ToString-able argument");
    assert(ranToString === 1 && ranValueOf === 0,
           "and it is toString that runs, not valueOf: " + ranToString + "/" + ranValueOf);

    /* A throwing coercion propagates and leaves the parser usable. */
    throws(() => us.parse({ toString() { throw new Error("boom"); } }), Error,
           "a throwing coercion propagates");
    eq(iso(us.parse("2026-07-28")), "2026-07-28T00:00:00Z", "and the parser still works");

    throws(() => us.parse(), TypeError, "parse with no argument");
    throws(() => DateParser.prototype.parse.call({}, "2026-07-28"), TypeError,
           "a foreign receiver");

    /* One instance, many parses, no state carried between them -- the whole
     * contract of a compiled capability. */
    const seq = ["2026-07-28", "in 3 days", "nonsense", "28 July 2026"];
    const first = seq.map(s => us.parse(s));
    const again = seq.map(s => us.parse(s));
    eq(JSON.stringify(first), JSON.stringify(again), "reuse is stateless");
}

/* ==================================================================== *
 *  7. Degenerate and boundary input
 * ==================================================================== */
{
    eq(iso(us.parse("1970-01-01")), "1970-01-01T00:00:00Z", "the epoch");
    eq(us.parse("1969-12-31") < 0, true, "before the epoch is negative");
    eq(iso(us.parse("1969-12-31")), "1969-12-31T00:00:00Z", "and round-trips");
    eq(iso(us.parse("2100-12-31")), "2100-12-31T00:00:00Z", "a non-leap century");
    /* A very long string of digits is not a date and must not run away. */
    eq(us.parse("1".repeat(10000)), null, "a 10k-digit run is refused");
    eq(us.parse("\0"), null, "a NUL byte is refused");
}

print("test_time_dateparser: all " + n + " assertions passed");
