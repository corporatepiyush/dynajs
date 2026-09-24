/* test_time_nsec_strict.js -- formatRFC3339's typed slots are TYPED.
 *
 * The defect this pins: the legacy `(nsec, utc)` second slot coerced every
 * scalar through ToNumber -- "abc" silently became 0 nsec, `true` became
 * 1 ns, `null`/`[]`/NaN became 0, `2.5` truncated to 2 -- while the declared
 * type is `number`. The bag `nsec` shared the same laxity, and so did the
 * rest of the surface: `sec` coerced "abc"/true/1e300 to 0 or 1, `utc`
 * coerced anything truthy, `offsetMinutes` coerced "330"/true/null.
 *
 * One policy now covers every typed slot of formatRFC3339 (and, through the
 * shared reader, the `{ offsetMinutes }` bags of formatUnix and Format):
 *
 *   - a non-number (string, boolean, null, object, symbol, BigInt) throws
 *     TypeError "<slot> must be a number";
 *   - a non-integer number (2.5, NaN, Infinity) throws RangeError "<slot>
 *     must be an integer";
 *   - a whole number with no int64 representation (1e300) throws RangeError
 *     "<slot> is out of range" -- it is a RANGE failure, and the old message
 *     called it a fraction;
 *   - a whole int64 value is range-checked by the slot's own bound: nsec in
 *     [0, 999999999] (message unchanged), offsetMinutes in [-1439, 1439];
 *   - `utc` takes a boolean or throws TypeError.
 *
 * The second argument stays the ONE overloaded slot: an OBJECT there is the
 * options bag (that is how the bag form is spelled) -- so "an object throws
 * TypeError" is true of the bag's `nsec` PROPERTY, not of an object in the
 * legacy position, which selects the bag form.
 *
 * Run: dynajs tests/test_time_nsec_strict.js
 */
import { formatRFC3339, formatUnix, Format, parseRFC3339 } from "dyna:time";


let n = 0;
function assert(c, msg) { n++; if (!c) throw new Error("assertion failed: " + msg); }
function throws(fn, kind, re, msg) {
    n++;
    let e = null;
    try { fn(); } catch (err) { e = err; }
    if (e === null) throw new Error(msg + ": expected a throw");
    if (kind && !(e instanceof kind))
        throw new Error(msg + ": wrong error type: " + e);
    if (re && !re.test(String(e.message)))
        throw new Error(msg + ": wrong message: " + e.message);
}

/* ---- 1. the legacy form keeps working for REAL NUMBERS ---- */
assert(formatRFC3339(0) === "1970-01-01T00:00:00Z", "no nsec: plain seconds");
assert(formatRFC3339(0, undefined) === "1970-01-01T00:00:00Z",
    "explicit undefined nsec == absent");
assert(formatRFC3339(0, 0) === "1970-01-01T00:00:00Z", "0 nsec");
assert(formatRFC3339(0, 500000000) === "1970-01-01T00:00:00.5Z", "half-second");
assert(formatRFC3339(0, 1) === "1970-01-01T00:00:00.000000001Z", "1 ns");
assert(formatRFC3339(0, 999999999) === "1970-01-01T00:00:00.999999999Z",
    "the 999999999 boundary");
assert(formatRFC3339(1234567890, 250000000, true) ===
       "2009-02-13T23:31:30.25Z", "the (nsec, utc) triple with utc=true");
const local = formatRFC3339(0, 7, false);
assert(/^19(69|70)-/.test(local) && !local.endsWith("00:00:00.000000007Z$"),
    "utc=false still renders at the local offset: " + local);
/* the pair still round-trips */
assert(parseRFC3339(formatRFC3339(0, 123456789)).nsec === 123456789,
    "round-trip keeps the nsec");

/* ---- 2. non-number scalars: TypeError, BOTH slots ---- */
for (const [slot, v] of [["legacy", "5"], ["legacy", "abc"], ["legacy", ""],
                         ["legacy", true], ["legacy", false], ["legacy", null],
                         ["legacy", 5n], ["legacy", Symbol("x")],
                         ["bag", "5"], ["bag", "abc"], ["bag", true],
                         ["bag", false], ["bag", null], ["bag", []],
                         ["bag", {}], ["bag", 5n]]) {
    throws(() => slot === "legacy" ? formatRFC3339(0, v)
                                   : formatRFC3339(0, { nsec: v }),
           TypeError, /nsec must be a number/,
           slot + " slot refuses " + String(v));
}
/* an object with a numeric valueOf is still a non-number: typing, not
   coercion, is the contract */
throws(() => formatRFC3339(0, { valueOf() { return 5; } }),
       TypeError, /unknown option/, "the bag stays strict on keys");
throws(() => formatRFC3339(0, { nsec: { valueOf() { return 5; } } }),
       TypeError, /nsec must be a number/, "a valueOf object is not a number");

/* OBJECT values in the second slot are BAG attempts, not legacy-nsec
   attempts: the bag form is (sec, opts) and takes any object. An empty
   object or array is the empty bag == UTC (pinned elsewhere); an array with
   entries has indexed keys the strict bag refuses. */
assert(formatRFC3339(0, {}) === "1970-01-01T00:00:00Z",
    "an empty object is the empty bag");
assert(formatRFC3339(0, []) === "1970-01-01T00:00:00Z",
    "an empty array is the empty bag");
throws(() => formatRFC3339(0, [5]), TypeError, /unknown option "0"/,
    "an array with entries refuses as a bag with unknown keys");

/* ---- 3. numbers that are not whole / not finite: RangeError ---- */
for (const [slot, v] of [["legacy", 2.5], ["legacy", -0.5], ["legacy", NaN],
                         ["legacy", Infinity], ["legacy", -Infinity],
                         ["bag", 2.5], ["bag", NaN], ["bag", Infinity]]) {
    throws(() => slot === "legacy" ? formatRFC3339(0, v)
                                   : formatRFC3339(0, { nsec: v }),
           RangeError, /integer/,
           slot + " slot refuses the non-integer " + v);
}

/* ---- 4. the range rows are unchanged ---- */
throws(() => formatRFC3339(0, -1), RangeError, /nsec must be in/,
    "legacy -1 refuses");
throws(() => formatRFC3339(0, 1e9), RangeError, /nsec must be in/,
    "legacy 1e9 refuses");
throws(() => formatRFC3339(0, { nsec: 1e9 }), RangeError, /nsec must be in/,
    "bag 1e9 refuses (message unchanged)");
throws(() => formatRFC3339(0, { nsec: -1 }), RangeError, /nsec must be in/,
    "bag -1 refuses");

/* ---- 5. the rest of the surface is untouched ---- */
assert(formatRFC3339(0, { offsetMinutes: 330 }) ===
       "1970-01-01T05:30:00+05:30", "offsetMinutes still renders");
throws(() => formatRFC3339(0, { offsetMinutes: 1.5 }), RangeError, /integer/,
    "offsetMinutes keeps its own integer rule");
throws(() => formatRFC3339(0, { nsec: 1, extra: 2 }), TypeError,
    /unknown option "extra"/, "the bag stays key-strict");
throws(() => formatRFC3339(0, { nsec: 1 }, 2), TypeError, /exactly 2/,
    "the bag form is still exactly two arguments");


/* ---- 6. the rest of the surface: sec and utc ---- */
/* sec is a whole number or nothing: the coercion this pins is "abc" -> 0s,
   true -> 1s, 1e300 -> 0s (all of which rendered 1970 as if the caller had
   asked for it). */
assert(formatRFC3339(0) === "1970-01-01T00:00:00Z", "0 is the epoch");
assert(formatRFC3339(-1) === "1969-12-31T23:59:59Z", "negative seconds still work");
assert(typeof formatRFC3339(1e15) === "string" &&
       /^\d{5,}-\d\d-\d\dT\d\d:\d\d:\d\dZ$/.test(formatRFC3339(1e15)),
    "a large representable second still renders: " + formatRFC3339(1e15));
for (const v of ["0", "abc", "", true, false, null, 5n, Symbol("x"), [], {}]) {
    throws(() => formatRFC3339(v), TypeError, /sec must be a number/,
        "sec refuses " + String(v));
}
for (const v of [2.5, -0.5, NaN, Infinity, -Infinity]) {
    throws(() => formatRFC3339(v), RangeError, /sec must be an integer/,
        "sec refuses the non-integer " + v);
}
for (const v of [1e300, -1e300, 9223372036854775808]) {
    throws(() => formatRFC3339(v), RangeError, /sec is out of range/,
        "sec refuses the out-of-int64 " + v);
}
/* the out-of-int64 message applies to nsec too (it used to claim the value
   was fractional), while 1e9 keeps the unchanged domain message */
throws(() => formatRFC3339(0, 1e300), RangeError, /nsec is out of range/,
    "legacy nsec 1e300 is out of range");
throws(() => formatRFC3339(0, { nsec: 1e300 }), RangeError, /nsec is out of range/,
    "bag nsec 1e300 is out of range");
throws(() => formatRFC3339(0, 9.3e18), RangeError, /nsec is out of range/,
    "legacy nsec above 2^63 is out of range");
/* utc is a boolean */
throws(() => formatRFC3339(0, 0, "true"), TypeError, /utc must be a boolean/,
    "utc refuses a string");
throws(() => formatRFC3339(0, 0, null), TypeError, /utc must be a boolean/,
    "utc refuses null");
throws(() => formatRFC3339(0, 0, 0), TypeError, /utc must be a boolean/,
    "utc refuses a number");
throws(() => formatRFC3339(0, 0, {}), TypeError, /utc must be a boolean/,
    "utc refuses an object");
assert(formatRFC3339(0, 0, undefined) === "1970-01-01T00:00:00Z",
    "an absent utc still means true");
assert(formatRFC3339(0, 0, true) === "1970-01-01T00:00:00Z", "utc=true");
/* utc=false renders at the machine's own offset: assert the round-trip
   instant, which is the same in every zone (the text is not). */
assert(parseRFC3339(formatRFC3339(0, 0, false)).sec === 0,
    "utc=false still renders a parseable local timestamp: " + formatRFC3339(0, 0, false));

/* ---- 7. the offsetMinutes bag is number-typed, in all three carriers ---- */
for (const v of ["330", "abc", true, false, null, [], {}, 5n]) {
    throws(() => formatRFC3339(0, { offsetMinutes: v }), TypeError,
        /offsetMinutes must be a number/,
        "formatRFC3339 offsetMinutes refuses " + String(v));
}
throws(() => formatRFC3339(0, { offsetMinutes: 1e300 }), RangeError,
    /offsetMinutes is out of range/, "offsetMinutes 1e300 is out of range");
throws(() => formatRFC3339(0, { offsetMinutes: 1440 }), RangeError, /-1439, 1439/,
    "an out-of-domain offset keeps its message");
assert(formatRFC3339(0, { offsetMinutes: 330 }) === "1970-01-01T05:30:00+05:30",
    "the real offset still renders");
throws(() => formatUnix(0, "%z", { offsetMinutes: "x" }), TypeError,
    /offsetMinutes must be a number/, "formatUnix shares the reader");
throws(() => formatUnix(0, "%z", { offsetMinutes: null }), TypeError,
    /offsetMinutes must be a number/, "formatUnix: null is not a number");
assert(formatUnix(0, "%z", undefined) === "Z", "an absent bag is still no options");
throws(() => new Format("%z", { offsetMinutes: true }), TypeError,
    /offsetMinutes must be a number/, "Format shares the reader");

/* ---- 8. an OBJECT in the second slot is the bag, never a nsec ---- */
assert(formatRFC3339(0, {}) === "1970-01-01T00:00:00Z",
    "the empty bag is UTC");
assert(formatRFC3339(0, { nsec: 5 }) === "1970-01-01T00:00:00.000000005Z",
    "the bag's nsec is read from an object in the second slot");
assert(formatRFC3339(0, new Number(5)) === "1970-01-01T00:00:00Z",
    "a boxed Number is an object: it selects the bag (empty), not a nsec");


print("test_time_nsec_strict: all " + n + " assertions passed");
