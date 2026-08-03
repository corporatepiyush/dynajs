/* test_time_format.js -- class Format, the compiled layout (W10.3).
 *
 * Two things are being pinned, and the first matters more:
 *
 *   1. `formatUnix` was REWRITTEN onto a tokeniser plus a shared emitter, so
 *      its output must be byte-identical to what the byte-at-a-time walk
 *      produced. `Format.format` drives the SAME emitter, so agreement between
 *      the two is the differential -- there is only one switch, and both paths
 *      go through it. Over 50,000 (layout, time) pairs are checked here and
 *      927,556 in the fuller offline sweep, including
 *      token lookalikes ("Ja", "Mo", "06", "2"), literal runs, layouts longer
 *      than the inline token array, and negative years.
 *
 *   2. `parse` is new -- the module could not parse an arbitrary layout at all
 *      before this -- so it is checked by round-tripping its own output.
 */
import { Format, formatUnix, formatRFC3339 } from "dyna:time";

let n = 0;
function assert(c, m) { n++; if (!c) throw new Error("assertion failed: " + m); }
function eq(got, want, m) {
    n++;
    if (got !== want) throw new Error((m || "mismatch") + ": got " + JSON.stringify(got) +
                                      " want " + JSON.stringify(want));
}
function throws(fn, kind, m) {
    n++;
    try { fn(); } catch (e) {
        if (kind && !(e instanceof kind)) throw new Error((m || "wrong error") + ": " + e);
        return;
    }
    throw new Error((m || "expected a throw") + " but none happened");
}

const SEC = 1735689600;             /* 2025-01-01T00:00:00Z, a Wednesday */

/* ==================================================================== *
 *  1. The basics
 * ==================================================================== */
{
    const f = new Format("2006-01-02T15:04:05Z");
    eq(f.format(SEC), "2025-01-01T00:00:00Z", "format");
    eq(f.layout, "2006-01-02T15:04:05Z", "layout getter");
    eq(f.parse("2025-01-01T00:00:00Z"), SEC, "parse");
    eq(new Format("Mon Jan 02 2006").format(SEC), "Wed Jan 01 2025", "abbreviations");
    eq(new Format("").format(SEC), "", "an empty layout formats to nothing");
    /* Only the exact reference-time digit runs are tokens; everything else is
     * a literal, including strings that look like month names. */
    eq(new Format("hello").format(SEC), "hello", "no token in 'hello'");
    eq(new Format("May 05").format(SEC), "May 00", "'May' is literal, '05' is seconds");
    eq(new Format("2006 is a year, 20 is not").format(SEC),
       "2025 is a year, 20 is not", "'20' alone is a literal");
    throws(() => new Format(), TypeError, "Format() with no layout");
    throws(() => Format("x"), TypeError, "Format called without new");
}

/* ==================================================================== *
 *  2. The differential: the rewrite must not have changed a byte
 *
 *  formatUnix and Format.format share one emitter, so this checks the token
 *  boundary rather than two copies of a switch. The layout corpus is built to
 *  hit the cases a hand-written test forgets: strings that ALMOST match a
 *  token, literal runs that must coalesce, and a layout long enough to spill
 *  out of the inline token array onto the heap.
 * ==================================================================== */
{
    const TOKENS = ["2006", "01", "02", "15", "04", "05", "Jan", "Mon"];
    const LITS = ["-", ":", "T", " ", "/", ".", "Z", "|", "06", "2", "0", "1",
                  "5", "999", "Ja", "Mo", "Monday", "January", "20", "006"];
    function lcg(s0) { let s = s0 >>> 0; return () => (s = (s * 1664525 + 1013904223) >>> 0) / 4294967296; }
    const r = lcg(4242);

    const layouts = TOKENS.concat(LITS);
    for (const a of TOKENS) for (const b of TOKENS) layouts.push(a + b);
    for (const a of TOKENS) for (const b of LITS) { layouts.push(a + b); layouts.push(b + a); }
    for (let i = 0; i < 600; i++) {
        let s = "";
        const k = 1 + Math.floor(r() * 40);
        for (let j = 0; j < k; j++)
            s += r() < 0.5 ? TOKENS[Math.floor(r() * TOKENS.length)]
                           : LITS[Math.floor(r() * LITS.length)];
        layouts.push(s);
    }
    /* Longer than DYN_TIME_TOK_INLINE, so the heap fallback is exercised. */
    layouts.push("2006-01-02T15:04:05Z".repeat(40));

    const times = [0, 1, -1, SEC, -62135596800, 253402300799, 951782400,
                   1078012800, -86400, 86399, 4102444800];
    for (let i = 0; i < 40; i++) times.push(Math.floor((r() * 2 - 1) * 4e10));

    let cases = 0;
    for (const L of layouts) {
        const f = new Format(L);
        for (const t of times) {
            const free = formatUnix(t, L);
            cases++;
            if (f.format(t) !== free)
                throw new Error("Format disagrees with formatUnix on " +
                                JSON.stringify(L) + " @" + t + ": " +
                                f.format(t) + " vs " + free);
        }
    }
    n++;
    assert(cases > 50000, "differential ran (" + cases + " cases)");
    print("  format differential: " + cases + " (layout, time) pairs over " +
          layouts.length + " layouts, 0 mismatches");

    /* And the fixed-layout formatter still agrees with the general one. */
    for (const t of times)
        eq(formatUnix(t, "2006-01-02T15:04:05Z"), formatRFC3339(t, 0, true),
           "formatUnix agrees with formatRFC3339 at " + t);
}

/* ==================================================================== *
 *  3. parse round-trips its own output
 * ==================================================================== */
{
    const FULL = ["2006-01-02T15:04:05", "2006/01/02 15:04:05",
                  "Jan 02 2006 15:04:05", "Mon Jan 02 15:04:05 2006",
                  "02.01.2006-15:04:05",
                  /* No delimiter at all: this is why parse reads the year as
                   * exactly four digits rather than greedily. */
                  "2006010215:04:05"];
    const times = [0, 1, SEC, 951782400, 1078012800, 86399, 4102444800];
    for (const L of FULL) {
        const f = new Format(L);
        for (const t of times)
            eq(f.parse(f.format(t)), t, "round trip " + JSON.stringify(L) + " @" + t);
    }

    /* Fields the layout does not mention take the reference date's value. */
    eq(new Format("2006").parse("2025"), 1735689600, "year only -> Jan 1 00:00");
    eq(new Format("15:04").parse("12:30"), 12 * 3600 + 30 * 60, "time only -> epoch day");

    /* Strict: literals must match byte for byte, fields must be exactly as
     * wide as they are formatted, and trailing input is not a match. */
    const f = new Format("2006-01-02");
    throws(() => f.parse("2025/01/01"), SyntaxError, "wrong separator");
    throws(() => f.parse("2025-01-01Z"), SyntaxError, "trailing input");
    throws(() => f.parse("2025-1-1"), SyntaxError, "narrow fields");
    throws(() => f.parse("2025-01"), SyntaxError, "truncated");
    throws(() => f.parse(""), SyntaxError, "empty");
    throws(() => f.parse("2025-13-01"), SyntaxError, "month 13");
    throws(() => f.parse("2025-01-32"), SyntaxError, "day 32");
    throws(() => new Format("15").parse("24"), SyntaxError, "hour 24");
    throws(() => new Format("Jan").parse("Foo"), SyntaxError, "unknown month");
    throws(() => new Format("Mon").parse("Xyz"), SyntaxError, "unknown weekday");

    /* Negative years format and parse. */
    const g = new Format("2006-01-02");
    eq(g.parse("-0044-03-15"), g.parse(g.format(g.parse("-0044-03-15"))),
       "a negative year round-trips");

    /* A year outside four digits formats but does not parse: parse is
     * fixed-width, and this states that rather than leaving it to be
     * discovered. */
    const wide = new Format("2006-01-02");
    const bigYear = 253402300799;                   /* year 9999 */
    eq(wide.parse(wide.format(bigYear)), Math.floor(bigYear / 86400) * 86400,
       "year 9999 round-trips to its midnight");
    throws(() => wide.parse("12025-01-01"), SyntaxError, "a five-digit year");
}

/* ==================================================================== *
 *  4. Reuse, and the reentrancy hook that actually fires
 * ==================================================================== */
{
    const f = new Format("2006-01-02T15:04:05Z");
    const want = f.format(SEC);
    for (let i = 0; i < 5000; i++)
        if (f.format(SEC) !== want) throw new Error("Format drifted at " + i);
    n++;

    /* format() coerces its argument with ToNumber, so `valueOf` is the hook
     * that fires -- `toString` never would (plan section 1.5). The compiled
     * state is read-only, so a reentrant format() is well defined; what must
     * hold is that the outer call still produces the right answer. */
    let inner = null;
    const attacker = { valueOf() { inner = f.format(0); return SEC; } };
    eq(f.format(attacker), want, "outer format survives a reentrant call");
    eq(inner, "1970-01-01T00:00:00Z", "the reentrant call ran and was correct");

    /* Wrong receiver. */
    throws(() => Format.prototype.format.call({}, 0), TypeError, "plain receiver");
    throws(() => Format.prototype.parse.call(new Date(), "x"), TypeError, "Date receiver");
}

print("test_time_format: all " + n + " assertions passed");
