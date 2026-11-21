// covers: dyna:time.formatRFC3339
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_time_format_rfc3339_nsec_strict");
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

__kitS.section("no nsec: plain seconds", () => {
assert(formatRFC3339(0) === "1970-01-01T00:00:00Z", "no nsec: plain seconds");
});

__kitS.section("explicit undefined nsec == absent", () => {
assert(formatRFC3339(0, undefined) === "1970-01-01T00:00:00Z",
    "explicit undefined nsec == absent");
});

__kitS.section("0 nsec", () => {
assert(formatRFC3339(0, 0) === "1970-01-01T00:00:00Z", "0 nsec");
});

__kitS.section("1970-01-01T00:00:00.5Z", () => {
assert(formatRFC3339(0, 500000000) === "1970-01-01T00:00:00.5Z", "half-second");
});

__kitS.section("1 ns", () => {
assert(formatRFC3339(0, 1) === "1970-01-01T00:00:00.000000001Z", "1 ns");
});

__kitS.section("the 999999999 boundary", () => {
assert(formatRFC3339(0, 999999999) === "1970-01-01T00:00:00.999999999Z",
    "the 999999999 boundary");
});

__kitS.section("the (nsec, utc) triple with utc=true", () => {
assert(formatRFC3339(1234567890, 250000000, true) ===
       "2009-02-13T23:31:30.25Z", "the (nsec, utc) triple with utc=true");
});

const local = formatRFC3339(0, 7, false);

__kitS.section("utc=false still renders at the local offset:", () => {
assert(/^19(69|70)-/.test(local) && !local.endsWith("00:00:00.000000007Z$"),
    "utc=false still renders at the local offset: " + local);
});

__kitS.section("slot refuses", () => {
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
});

__kitS.section("the bag stays strict on keys", () => {
throws(() => formatRFC3339(0, { valueOf() { return 5; } }),
       TypeError, /unknown option/, "the bag stays strict on keys");
});

__kitS.section("a valueOf object is not a number", () => {
throws(() => formatRFC3339(0, { nsec: { valueOf() { return 5; } } }),
       TypeError, /nsec must be a number/, "a valueOf object is not a number");
});

__kitS.section("an empty object is the empty bag", () => {
assert(formatRFC3339(0, {}) === "1970-01-01T00:00:00Z",
    "an empty object is the empty bag");
});

__kitS.section("an empty array is the empty bag", () => {
assert(formatRFC3339(0, []) === "1970-01-01T00:00:00Z",
    "an empty array is the empty bag");
});

__kitS.section("an array with entries refuses as a bag with unknown keys", () => {
throws(() => formatRFC3339(0, [5]), TypeError, /unknown option "0"/,
    "an array with entries refuses as a bag with unknown keys");
});

__kitS.section("slot refuses the non-integer", () => {
for (const [slot, v] of [["legacy", 2.5], ["legacy", -0.5], ["legacy", NaN],
                         ["legacy", Infinity], ["legacy", -Infinity],
                         ["bag", 2.5], ["bag", NaN], ["bag", Infinity]]) {
    throws(() => slot === "legacy" ? formatRFC3339(0, v)
                                   : formatRFC3339(0, { nsec: v }),
           RangeError, /integer/,
           slot + " slot refuses the non-integer " + v);
}
});

__kitS.section("legacy -1 refuses", () => {
throws(() => formatRFC3339(0, -1), RangeError, /nsec must be in/,
    "legacy -1 refuses");
});

__kitS.section("legacy 1e9 refuses", () => {
throws(() => formatRFC3339(0, 1e9), RangeError, /nsec must be in/,
    "legacy 1e9 refuses");
});

__kitS.section("bag 1e9 refuses (message unchanged)", () => {
throws(() => formatRFC3339(0, { nsec: 1e9 }), RangeError, /nsec must be in/,
    "bag 1e9 refuses (message unchanged)");
});

__kitS.section("bag -1 refuses", () => {
throws(() => formatRFC3339(0, { nsec: -1 }), RangeError, /nsec must be in/,
    "bag -1 refuses");
});

__kitS.section("offsetMinutes still renders", () => {
assert(formatRFC3339(0, { offsetMinutes: 330 }) ===
       "1970-01-01T05:30:00+05:30", "offsetMinutes still renders");
});

__kitS.section("offsetMinutes keeps its own integer rule", () => {
throws(() => formatRFC3339(0, { offsetMinutes: 1.5 }), RangeError, /integer/,
    "offsetMinutes keeps its own integer rule");
});

__kitS.section("the bag stays key-strict", () => {
throws(() => formatRFC3339(0, { nsec: 1, extra: 2 }), TypeError,
    /unknown option "extra"/, "the bag stays key-strict");
});

throws(() => formatRFC3339(0, { nsec: 1 }, 2), TypeError, /exactly 2/,
    "the bag form is still exactly two arguments");

__kitS.section("0 is the epoch", () => {
assert(formatRFC3339(0) === "1970-01-01T00:00:00Z", "0 is the epoch");
});

__kitS.section("negative seconds still work", () => {
assert(formatRFC3339(-1) === "1969-12-31T23:59:59Z", "negative seconds still work");
});

__kitS.section("a large representable second still renders:", () => {
assert(typeof formatRFC3339(1e15) === "string" &&
       /^\d{5,}-\d\d-\d\dT\d\d:\d\d:\d\dZ$/.test(formatRFC3339(1e15)),
    "a large representable second still renders: " + formatRFC3339(1e15));
});

__kitS.section("sec refuses", () => {
for (const v of ["0", "abc", "", true, false, null, 5n, Symbol("x"), [], {}]) {
    throws(() => formatRFC3339(v), TypeError, /sec must be a number/,
        "sec refuses " + String(v));
}
});

__kitS.section("sec refuses the non-integer", () => {
for (const v of [2.5, -0.5, NaN, Infinity, -Infinity]) {
    throws(() => formatRFC3339(v), RangeError, /sec must be an integer/,
        "sec refuses the non-integer " + v);
}
});

__kitS.section("sec refuses the out-of-int64", () => {
for (const v of [1e300, -1e300, 9223372036854775808]) {
    throws(() => formatRFC3339(v), RangeError, /sec is out of range/,
        "sec refuses the out-of-int64 " + v);
}
});

__kitS.section("legacy nsec 1e300 is out of range", () => {
throws(() => formatRFC3339(0, 1e300), RangeError, /nsec is out of range/,
    "legacy nsec 1e300 is out of range");
});

__kitS.section("bag nsec 1e300 is out of range", () => {
throws(() => formatRFC3339(0, { nsec: 1e300 }), RangeError, /nsec is out of range/,
    "bag nsec 1e300 is out of range");
});

__kitS.section("legacy nsec above 2^63 is out of range", () => {
throws(() => formatRFC3339(0, 9.3e18), RangeError, /nsec is out of range/,
    "legacy nsec above 2^63 is out of range");
});

__kitS.section("utc refuses a string", () => {
throws(() => formatRFC3339(0, 0, "true"), TypeError, /utc must be a boolean/,
    "utc refuses a string");
});

__kitS.section("utc refuses null", () => {
throws(() => formatRFC3339(0, 0, null), TypeError, /utc must be a boolean/,
    "utc refuses null");
});

__kitS.section("utc refuses a number", () => {
throws(() => formatRFC3339(0, 0, 0), TypeError, /utc must be a boolean/,
    "utc refuses a number");
});

__kitS.section("utc refuses an object", () => {
throws(() => formatRFC3339(0, 0, {}), TypeError, /utc must be a boolean/,
    "utc refuses an object");
});

__kitS.section("an absent utc still means true", () => {
assert(formatRFC3339(0, 0, undefined) === "1970-01-01T00:00:00Z",
    "an absent utc still means true");
});

__kitS.section("1970-01-01T00:00:00Z", () => {
assert(formatRFC3339(0, 0, true) === "1970-01-01T00:00:00Z", "utc=true");
});

__kitS.section("utc=false still renders a parseable local timestamp:", () => {
assert(parseRFC3339(formatRFC3339(0, 0, false)).sec === 0,
    "utc=false still renders a parseable local timestamp: " + formatRFC3339(0, 0, false));
});

__kitS.section("formatRFC3339 offsetMinutes refuses", () => {
for (const v of ["330", "abc", true, false, null, [], {}, 5n]) {
    throws(() => formatRFC3339(0, { offsetMinutes: v }), TypeError,
        /offsetMinutes must be a number/,
        "formatRFC3339 offsetMinutes refuses " + String(v));
}
});

__kitS.section("offsetMinutes 1e300 is out of range", () => {
throws(() => formatRFC3339(0, { offsetMinutes: 1e300 }), RangeError,
    /offsetMinutes is out of range/, "offsetMinutes 1e300 is out of range");
});

__kitS.section("an out-of-domain offset keeps its message", () => {
throws(() => formatRFC3339(0, { offsetMinutes: 1440 }), RangeError, /-1439, 1439/,
    "an out-of-domain offset keeps its message");
});

__kitS.section("the real offset still renders", () => {
assert(formatRFC3339(0, { offsetMinutes: 330 }) === "1970-01-01T05:30:00+05:30",
    "the real offset still renders");
});

__kitS.section("the empty bag is UTC", () => {
assert(formatRFC3339(0, {}) === "1970-01-01T00:00:00Z",
    "the empty bag is UTC");
});

__kitS.section("the bag's nsec is read from an object in the second slot", () => {
assert(formatRFC3339(0, { nsec: 5 }) === "1970-01-01T00:00:00.000000005Z",
    "the bag's nsec is read from an object in the second slot");
});

__kitS.section("a boxed Number is an object: it selects the bag (empty), not a nsec", () => {
assert(formatRFC3339(0, new Number(5)) === "1970-01-01T00:00:00Z",
    "a boxed Number is an object: it selects the bag (empty), not a nsec");
});
__kitS.end();

print("test_time_nsec_strict: all " + n + " assertions passed");
