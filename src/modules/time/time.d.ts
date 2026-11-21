/// <reference path="../../../src/types/prelude.d.ts" />
//@dts-block dyna:time
/** dyna:time — clocks, RFC 3339 and Go-style layouts, durations, civil types (PlainDate/PlainTime/PlainDateTime), time zones, RFC 5545 recurrence (RRule). */
declare module "dyna:time" {
    // POSIX only. Clocks: now()/nowSec()/nowMillis()/nowUnixNano() read the WALL clock; monotonicNano() is MONOTONIC — use it to measure durations.

    /** Go-style duration units in NANOSECONDS (Second === 1e9): multiply to build the values parseDuration returns. */
    const Nanosecond: number;
    const Microsecond: number;
    const Millisecond: number;
    const Second: number;
    const Minute: number;
    const Hour: number;

    /** Parses "300ms", "-1.5h", "2h45m" into a count of NANOSECONDS: a `number` below 2^53 ns, at or above 2^53 ns a `bigint`. Units ns/us/ms/s/m/h; "d" is NOT accepted. */
    function parseDuration(str: string): number | bigint;
    /** parseDuration in MILLISECONDS, always a `number` (the correctly-rounded double of ns/1e6). */
    function parseDurationMs(str: string): number;
    /** parseDuration in SECONDS, always a `number`. */
    function parseDurationSecs(str: string): number;
    /** The inverse of parseDuration; 0 is "0s" ("2h45m" round-trips as "2h45m0s"). */
    function durationString(ns: number | bigint): string;

    /** A calendar duration; years fold into months and weeks into days. */
    class Duration {
        /** Components are limited to |value| <= 1e12 (RangeError beyond; keeps downstream int64 folds in range). */
        constructor(opts: { years?: number; months?: number; weeks?: number; days?: number; hours?: number; minutes?: number; seconds?: number; milliseconds?: number });
        readonly years: number;
        readonly months: number;
        /** The day count. */
        readonly days: number;
        /** 1, -1 or 0. */
        readonly sign: number;
        /** True when every component is zero. */
        readonly blank: boolean;
        /** ISO 8601; a mixed-sign value throws. */
        toString(): string;
    }

    /** `new Duration({ milliseconds: ms })` as a call (non-integers truncate toward zero). */
    function durationMs(ms: number | bigint): Duration;
    /** `new Duration({ seconds: s })` as a call; fractional input truncates to whole seconds FIRST (use durationMs for sub-second precision). */
    function durationSecs(s: number | bigint): Duration;

    /** {sec, nsec} from CLOCK_REALTIME (wall time). */
    function now(): { sec: number; nsec: number };
    /** Whole seconds since the Unix epoch (wall time) -- now().sec as one number. */
    function nowSec(): number;
    /** BigInt nanoseconds since the Unix epoch (wall time); int64, so it ends in 2262. */
    function nowUnixNano(): bigint;
    /** Alias of nowUnixNano: the same clock under the duration-side name. */
    function nowNanos(): bigint;
    /** Milliseconds since the Unix epoch (wall time), like Date.now(). */
    function nowMillis(): number;
    /** BigInt nanoseconds from CLOCK_MONOTONIC (arbitrary origin): durations only, never compared across processes. */
    function monotonicNano(): bigint;

    /** RFC 3339 text for unix `sec`: the legacy tail (nsec, utc), or a strict bag { nsec, offsetMinutes } rendering at a fixed offset (-1439..1439). Slots are typed, never coerced. */
    function formatRFC3339(sec: number, nsec?: number, utc?: boolean): string;
    function formatRFC3339(sec: number, opts: { nsec?: number; offsetMinutes?: number }): string;
    /** Formats with Go-style layout tokens (2006 January Jan Monday Mon 01 02 15 04 05) plus %z; as in Go, the long names win where they match ('Monday' is the full weekday, never 'Mon' + 'day'); Format.parse matches names case-sensitively; { offsetMinutes } fixes the offset (default UTC). */
    function formatUnix(sec: number, layout: string, opts?: { offsetMinutes?: number }): string;
    /** Strict RFC 3339 parse; returns {sec, nsec}. */
    function parseRFC3339(str: string): { sec: number; nsec: number };
    /** Unix seconds (UTC) from civil fields; an out-of-range month carries; |day| <= 1e9 and |hour/minute/second| <= 1e6, else RangeError. */
    function date(y: number, mo: number, d: number, h?: number, mi?: number, s?: number): number;
    /** {year, month, day, hour, min, sec, weekday, yday}; weekday 0 = Sunday. */
    function fromUnix(sec: number): { year: number; month: number; day: number; hour: number; min: number; sec: number; weekday: number; yday: number };

    /** A compiled Go-style layout; { offsetMinutes } fixes the offset for format() and for parse() unless the input's own %z supplies one. */
    class Format {
        constructor(layout: string, opts?: { offsetMinutes?: number });
        /** Formats unix seconds with this layout. */
        format(sec: number): string;
        /** The strict inverse; omitted fields default to 1970-01-01T00:00:00Z; an unsupported year is a SyntaxError. */
        parse(str: string): number;
        /** The original layout. */
        readonly layout: string;
    }

    /** An immutable calendar date, proleptic Gregorian. */
    class PlainDate {
        /** All three are required. An impossible date is refused, never rolled over: `31 February` throws. */
        constructor(year: number, month: number, day: number);
        readonly year: number;
        readonly month: number;
        readonly day: number;
        /** ISO 8601, Monday is 1. */
        readonly dayOfWeek: number;
        readonly dayOfYear: number;
        readonly daysInMonth: number;
        readonly daysInYear: number;
        readonly inLeapYear: boolean;
        /** Days since 1970-01-01. */
        readonly epochDay: number;
        /** Months move first and clamp, then days are added exactly. */
        add(duration: Duration): PlainDate;
        subtract(duration: Duration): PlainDate;
        /** A Duration of whole months plus the remaining days. */
        until(other: PlainDate): Duration;
        /** `-1`, `0` or `1`. */
        compare(other: PlainDate): -1 | 0 | 1;
        /** ISO 8601; years outside 0..9999 print signed with six digits. */
        toString(): string;
    }

    /** A date and time of day with no zone; adding time carries into the date. */
    class PlainDateTime {
        constructor(year: number, month: number, day: number, hour?: number, minute?: number, second?: number, millisecond?: number);
        readonly year: number;
        readonly month: number;
        readonly day: number;
        readonly hour: number;
        readonly minute: number;
        readonly second: number;
        readonly millisecond: number;
        readonly epochDay: number;
        readonly dayOfWeek: number;
        add(duration: Duration): PlainDateTime;
        subtract(duration: Duration): PlainDateTime;
        /** The Duration from this to `other`: whole months first, then days, then the time remainder in ms, so a.add(a.until(b)) equals b exactly. */
        until(other: PlainDateTime): Duration;
        toPlainDate(): PlainDate;
        toPlainTime(): PlainTime;
        /** `-1`, `0` or `1`. */
        compare(other: PlainDateTime): -1 | 0 | 1;
        /** ISO 8601 with `T`, milliseconds omitted when zero. */
        toString(): string;
    }

    /** A wall-clock time of day; one integer millisecond count since midnight. */
    class PlainTime {
        /** `24:00` is refused (it names the same wall clock as `00:00`). */
        constructor(hour?: number, minute?: number, second?: number, millisecond?: number);
        readonly hour: number;
        readonly minute: number;
        readonly second: number;
        readonly millisecond: number;
        /** The whole value. */
        readonly msSinceMidnight: number;
        /** Adds the time part, wrapping at midnight; a duration carrying MONTHS is a RangeError, weeks/days are ignored. */
        add(duration: Duration): PlainTime;
        subtract(duration: Duration): PlainTime;
        /** `-1`, `0` or `1`. */
        compare(other: PlainTime): -1 | 0 | 1;
        /** `"HH:MM:SS"`, milliseconds appended when non-zero. */
        toString(): string;
    }

    /** RFC 5545 recurrence rules, UTC whole-second unix time. */
    interface RRuleOptions {
        freq: "YEARLY" | "MONTHLY" | "WEEKLY" | "DAILY" | "HOURLY" | "MINUTELY" | "SECONDLY";
        interval?: number;
        /** count and until are MUTUALLY EXCLUSIVE (RFC 5545): both set is a TypeError (options and string parsers alike). */
        count?: number;
        until?: Date | string | number;
        dtstart?: Date | string | number;
        wkst?: number | "MO" | "TU" | "WE" | "TH" | "FR" | "SA" | "SU";
        bymonth?: number[];
        bymonthday?: number[];
        byyearday?: number[];
        byweekno?: number[];
        bysetpos?: number[];
        byweekday?: (string | number)[];
    }
    /** An RFC 5545 recurrence rule, expanded into Dates on demand. */
    class RRule {
        /** `BYHOUR`/`BYMINUTE`/`BYSECOND`/`BYEASTER` are refused rather than silently ignored. */
        constructor(opts: RRuleOptions);
        /** Parses "RRULE:FREQ=..." parts plus optional DTSTART: lines; `opts.dtstart` supplies the start otherwise. */
    static fromString(str: string, opts?: { dtstart?: Date | string | number }): RRule;
        /** Every occurrence as Dates (`limit` caps the count; an uncounted infinite rule refuses). Expansion has a budget of 2^28 day/position steps (RangeError). */
        all(limit?: number): Date[];
        /** Occurrences in the window; inc makes both ends inclusive. Always equal to all() filtered to the window; a rule without COUNT is not expanded from DTSTART but from the period holding `start`, reached by whole INTERVALs of the rule's period counted from DTSTART, and the expansion stops at the period holding `end`. */
        between(start: Date | number, end: Date | number, inc?: boolean): Date[];
        /** The first occurrence strictly after fromDate, or null; found by the same seek as between(). */
        next(fromDate?: Date | number): Date | null;
        /** The last occurrence strictly before fromDate. */
        prev(fromDate?: Date | number): Date | null;
        /** The rule back in RFC 5545 text form. */
        toString(): string;
    }

    /** Natural-language date parsing per locale. */
    class DateParser {
        /** `opts.now` pins the reference time; it is the only valid key. */
        constructor(locale?: string, opts?: { now?: number });
        /** Unix seconds, or null when nothing matches. */
        parse(text: string): number | null;
        readonly locale: string;
        readonly dayFirst: boolean;
    }

    /** Strict ISO "YYYY-MM-DD" parse (parseTime rejects a seconds field of 60; the RFC 3339 parsers fold it into the next minute). */
    function parseDate(text: string): PlainDate;
    /** The PlainDate `n` days after 1970-01-01. */
    function dateFromEpochDay(n: number): PlainDate;
    /** Strict parse of "HH:MM[:SS[.mmm]]". */
    function parseTime(text: string): PlainTime;
    /** Joins a PlainDate and a PlainTime by pure field copy (the inverse of toPlainDate()/toPlainTime()). */
    function toPlainDateTime(date: PlainDate, time: PlainTime): PlainDateTime;
}
