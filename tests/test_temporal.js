/* PlainDate and Duration.
   The oracle is JS Date in UTC -- an independent Gregorian implementation
   already in the runtime, not a round trip through PlainDate itself. */
import { PlainDate, Duration, PlainTime, PlainDateTime, parseDate,
         dateFromEpochDay, parseTime } from "dyna:time";

let n = 0, bad = 0;
function eq(a, b, m) { n++; if (a !== b) { bad++; print("FAIL " + m + ": got " + a + ", want " + b); } }
function threw(fn, re, m) {
    n++;
    try { fn(); bad++; print("FAIL " + m + ": did not throw"); }
    catch (e) { if (re && !re.test(e.message)) { bad++; print("FAIL " + m + ": " + e.message); } }
}

/* --- differential against Date, over 40 years of consecutive days --- */
{
    let mismatched = 0, checked = 0;
    for (let ed = -3653; ed < 14610; ed += 1) {      /* 1960-01-01 .. 2010 */
        const p = dateFromEpochDay(ed);
        const j = new Date(ed * 86400000);
        checked++;
        if (p.year !== j.getUTCFullYear() || p.month !== j.getUTCMonth() + 1
            || p.day !== j.getUTCDate()
            || p.dayOfWeek !== (j.getUTCDay() === 0 ? 7 : j.getUTCDay())
            || p.epochDay !== ed) {
            if (++mismatched < 4)
                print("FAIL day " + ed + ": " + p.toString() + " vs "
                      + j.toISOString().slice(0, 10));
        }
    }
    n++; if (mismatched) bad++;
    eq(checked, 18263, "every day in the range was checked");
    eq(mismatched, 0, "no calendar mismatch against Date");
}

/* --- leap-year rules, including the century cases Date agrees on --- */
eq(new PlainDate(2000, 2, 29).toString(), "2000-02-29", "2000 is a leap year");
eq(new PlainDate(2024, 2, 1).daysInMonth, 29, "February 2024 has 29 days");
eq(new PlainDate(1900, 2, 1).daysInMonth, 28, "1900 is NOT a leap year");
eq(new PlainDate(2100, 2, 1).daysInMonth, 28, "nor is 2100");
threw(() => new PlainDate(1900, 2, 29), /has no day 29/, "1900-02-29");
threw(() => new PlainDate(2023, 2, 29), /has no day 29/, "2023-02-29");
threw(() => new PlainDate(2023, 4, 31), /has no day 31/, "31 April");
threw(() => new PlainDate(2023, 13, 1), /month 13/, "month 13");
threw(() => new PlainDate(2023, 0, 1), /month 0/, "month 0");
threw(() => new PlainDate(2023, 1, 0), /has no day 0/, "day 0");
threw(() => new PlainDate(2023), /all three are required/, "too few arguments");

/* --- month arithmetic CLAMPS; it does not roll over. 31 Jan + 1 month is the
   last day of February, because 31 February is not a date. A library that
   rolls it to 3 March has silently answered a different question. --- */
eq(new PlainDate(2024, 1, 31).add(new Duration({ months: 1 })).toString(),
   "2024-02-29", "31 Jan + 1 month clamps to 29 Feb in a leap year");
eq(new PlainDate(2023, 1, 31).add(new Duration({ months: 1 })).toString(),
   "2023-02-28", "and to 28 Feb otherwise");
eq(new PlainDate(2024, 2, 29).add(new Duration({ years: 1 })).toString(),
   "2025-02-28", "29 Feb + 1 year clamps");
eq(new PlainDate(2024, 3, 31).subtract(new Duration({ months: 1 })).toString(),
   "2024-02-29", "subtract clamps the same way");

/* Order is part of the contract: months then days, and the two orders differ. */
{   /* 31 Jan is NOT a witness: both orders land on 1 March. 30 Jan is --
       (30 Jan + 1 month) clamps to 29 Feb then +1 day is 1 March, while
       (30 Jan + 1 day) is 31 Jan and +1 month clamps to 29 Feb. */
    const d = new PlainDate(2024, 1, 30);
    const a = d.add(new Duration({ months: 1, days: 1 }));
    const b = d.add(new Duration({ days: 1 })).add(new Duration({ months: 1 }));
    eq(a.toString(), "2024-03-01", "months first, then days");
    eq(b.toString(), "2024-02-29", "days first gives a different date");
}

/* --- negative years and the proleptic calendar --- */
eq(new PlainDate(1, 1, 1).epochDay, -719162, "0001-01-01");
eq(dateFromEpochDay(-719162).toString(), "0001-01-01", "and back");
eq(new PlainDate(-1, 1, 1).toString(), "-000001-01-01",
   "a year outside 0..9999 is written expanded, never ambiguously");
eq(new PlainDate(12345, 6, 7).toString(), "+012345-06-07", "and above 9999");

/* --- until --- */
eq(new PlainDate(2024, 1, 31).until(new PlainDate(2024, 3, 30)).toString(),
   "P1M30D", "until spans months then days");
eq(new PlainDate(2024, 1, 1).until(new PlainDate(2024, 1, 1)).toString(),
   "P0D", "a zero span");
eq(new PlainDate(2024, 3, 1).until(new PlainDate(2024, 1, 1)).sign, -1,
   "a backwards span is negative");

/* --- compare --- */
eq(new PlainDate(2024, 1, 1).compare(new PlainDate(2024, 1, 2)), -1, "before");
eq(new PlainDate(2024, 1, 2).compare(new PlainDate(2024, 1, 1)), 1, "after");
eq(new PlainDate(2024, 1, 1).compare(new PlainDate(2024, 1, 1)), 0, "equal");

/* --- parsing refuses everything ISO 8601 does not permit --- */
eq(parseDate("2024-02-29").toString(), "2024-02-29", "a valid date");
eq(parseDate("+012345-06-07").toString(), "+012345-06-07", "an expanded year");
eq(parseDate("-000001-01-01").toString(), "-000001-01-01", "a negative year");
threw(() => parseDate("2024-2-9"), /not an ISO 8601/, "unpadded fields");
threw(() => parseDate("2024/02/29"), /not an ISO 8601/, "slashes");
threw(() => parseDate("2024-02-29T00:00"), /not an ISO 8601/, "trailing time");
threw(() => parseDate(" 2024-02-29"), /not an ISO 8601/, "leading space");
threw(() => parseDate("0x10-02-29"), /not an ISO 8601/, "a hex-looking year");
threw(() => parseDate("2023-02-29"), /has no day 29/, "a well-formed impossible date");
threw(() => parseDate(""), /not an ISO 8601/, "empty");

/* --- Duration --- */
eq(new Duration({ years: 1, months: 2, weeks: 3, days: 4 }).toString(), "P1Y2M25D",
   "weeks fold into days and years into months, both exactly");
eq(new Duration({}).toString(), "P0D", "an empty duration");
eq(new Duration({}).blank, true, "and it is blank");
eq(new Duration({ months: -3 }).toString(), "-P3M", "a negative duration");
eq(new Duration({ months: 14 }).years, 1, "years are months / 12");
threw(() => new Duration(), /options object/, "no argument");
threw(() => new PlainDate(2024, 1, 1).add(42), /Duration/, "adding a non-Duration");

/* --- PlainTime: one integer count of milliseconds since midnight, so every
   comparison is an integer compare and every result is exact. --- */
{
    const t = new PlainTime(9, 30, 15, 250);
    eq(t.toString(), "09:30:15.250", "toString keeps milliseconds");
    eq(t.hour, 9, "hour"); eq(t.minute, 30, "minute");
    eq(t.second, 15, "second"); eq(t.millisecond, 250, "millisecond");
    eq(t.msSinceMidnight, 34215250, "milliseconds since midnight");
    eq(new PlainTime(9, 30).toString(), "09:30:00",
       "seconds are always printed, so the shape never depends on the value");
}
/* Time WRAPS at midnight -- a time of day has nowhere to put the overflow,
   and discarding it silently would make the result quietly wrong. */
eq(new PlainTime(23, 30).add(new Duration({ hours: 2 })).toString(), "01:30:00",
   "adding across midnight wraps");
eq(new PlainTime(0, 30).subtract(new Duration({ hours: 2 })).toString(), "22:30:00",
   "and subtracting wraps back");
eq(new PlainTime(0, 0).subtract(new Duration({ milliseconds: 1 })).toString(),
   "23:59:59.999", "one millisecond before midnight");
eq(new PlainTime(12, 0).add(new Duration({ days: 1 })).toString(), "12:00:00",
   "a whole day returns the same time of day");

eq(new PlainTime(1, 0).compare(new PlainTime(2, 0)), -1, "time compare before");
eq(new PlainTime(2, 0).compare(new PlainTime(2, 0)), 0, "time compare equal");

/* 24:00 is a legal instant only as the END of a day; accepting it here would
   let two unequal values name the same wall clock. */
threw(() => new PlainTime(24, 0), /hour 24/, "24:00");
threw(() => new PlainTime(0, 60), /minute 60/, "minute 60");
threw(() => new PlainTime(0, 0, 60), /second 60/, "second 60 (no leap seconds)");
threw(() => new PlainTime(0, 0, 0, 1000), /millisecond 1000/, "millisecond 1000");
threw(() => new PlainTime(-1), /hour -1/, "a negative hour");
/* Months cannot be converted to hours, so the operation is refused rather
   than assuming a month length. */
threw(() => new PlainTime(1, 0).add(new Duration({ months: 1 })), /months has no meaning/,
      "a month added to a time of day");

eq(parseTime("07:05").toString(), "07:05:00", "HH:MM");
eq(parseTime("07:05:09").toString(), "07:05:09", "HH:MM:SS");
eq(parseTime("07:05:09.120").toString(), "07:05:09.120", "HH:MM:SS.mmm");
threw(() => parseTime("7:05"), /HH:MM/, "an unpadded hour");
threw(() => parseTime("07:05:09.12"), /HH:MM/, "two-digit milliseconds");
threw(() => parseTime("25:00"), /out of range/, "hour 25 parses but is refused");
threw(() => parseTime("07:05 "), /HH:MM/, "trailing space");

/* Duration's time part: hours/minutes/seconds fold into milliseconds exactly,
   but days do NOT fold into hours -- that assumes every day is 24 hours. */
eq(new Duration({ hours: 1, minutes: 2, seconds: 3, milliseconds: 4 }).toString(),
   "PT1H2M3.004S", "a time-only duration");
eq(new Duration({ days: 2, hours: 3 }).toString(), "P2DT3H",
   "days and hours stay on opposite sides of the T");
eq(new Duration({ milliseconds: 0 }).blank, true, "a zero time part is blank");
eq(new Duration({ seconds: -5 }).sign, -1, "a negative time-only duration");

/* --- PlainDateTime: the reason it is a type and not a pair is that time
   overflow CARRIES into the date, where PlainTime wraps and loses the day. --- */
{
    const dt = new PlainDateTime(2024, 3, 1, 23, 30);
    eq(dt.toString(), "2024-03-01T23:30:00", "toString is date T time");
    eq(dt.add(new Duration({ hours: 2 })).toString(), "2024-03-02T01:30:00",
       "adding across midnight carries into the date");
    eq(new PlainTime(23, 30).add(new Duration({ hours: 2 })).toString(), "01:30:00",
       "the same addition on a PlainTime wraps and loses the day");
}
/* Floor division, not truncation: C's / rounds toward zero, which would leave
   a negative time of day instead of carrying a whole day back. */
eq(new PlainDateTime(2024, 3, 1, 0, 0).subtract(new Duration({ milliseconds: 1 }))
     .toString(), "2024-02-29T23:59:59.999",
   "one millisecond before midnight crosses back over the leap day");
eq(new PlainDateTime(2024, 2, 28, 23, 0).add(new Duration({ hours: 2 })).toString(),
   "2024-02-29T01:00:00", "the carry lands on 29 February in a leap year");
eq(new PlainDateTime(2023, 2, 28, 23, 0).add(new Duration({ hours: 2 })).toString(),
   "2023-03-01T01:00:00", "and skips it otherwise");
eq(new PlainDateTime(2024, 1, 31, 12, 0).add(new Duration({ months: 1 })).toString(),
   "2024-02-29T12:00:00", "month arithmetic still clamps, time untouched");
eq(new PlainDateTime(2024, 1, 1, 0, 0).subtract(new Duration({ days: 1 })).toString(),
   "2023-12-31T00:00:00", "back across a year boundary");

eq(new PlainDateTime(2024, 3, 1, 23, 30).toPlainDate().toString(), "2024-03-01",
   "toPlainDate");
eq(new PlainDateTime(2024, 3, 1, 23, 30).toPlainTime().toString(), "23:30:00",
   "toPlainTime");
eq(new PlainDateTime(2024, 3, 1, 23, 30).dayOfWeek, 5, "dayOfWeek delegates");
eq(new PlainDateTime(2024, 3, 1, 1, 0).compare(new PlainDateTime(2024, 3, 1, 2, 0)),
   -1, "compare orders by instant, not by field");
eq(new PlainDateTime(2024, 3, 1, 23, 0).compare(new PlainDateTime(2024, 3, 2, 1, 0)),
   -1, "and across a date boundary");
eq(new PlainDateTime(2024, 1, 1).toString(), "2024-01-01T00:00:00",
   "the time defaults to midnight");

threw(() => new PlainDateTime(2023, 2, 29), /has no day 29/, "an impossible date");
threw(() => new PlainDateTime(2024, 1, 1, 24), /hour 24/, "hour 24");
threw(() => new PlainDateTime(2024, 1, 1, 0, 60), /minute 60/, "minute 60");
threw(() => new PlainDateTime(2024), /date is required/, "too few arguments");

if (bad) { print("test_temporal: " + bad + " FAILED of " + n); throw new Error("test_temporal failed"); }
print("test_temporal: " + n + " assertions, 0 failures");
