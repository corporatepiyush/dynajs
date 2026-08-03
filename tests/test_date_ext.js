/* test_date_ext.js — Date.prototype extensions. Local-time, immutable.
 * Uses local-constructed dates (self-consistent across timezones). Run: dynajs tests/test_date_ext.js */
let n = 0;
function assert(c,m){ n++; if(!c) throw new Error("assert: "+m); }
function eq(a,b,m){ n++; if(a!==b) throw new Error("eq "+m+" got "+JSON.stringify(a)+" want "+JSON.stringify(b)); }

const leap = new Date(2024,1,29,15,30,45,500);   // Thu Feb 29 2024 (leap)
const jan1 = new Date(2024,0,1);                 // Mon Jan 1 2024
const jan11 = new Date(2024,0,11);

/* predicates */
assert(leap.isValid() && !new Date(NaN).isValid(), "isValid");
assert(leap.isLeapYear() && !new Date(2023,0,1).isLeapYear(), "isLeapYear");
assert(leap.isThursday() && !leap.isMonday(), "day-of-week");
assert(leap.isFebruary() && !leap.isJanuary(), "month");
assert(jan1.isMonday() && jan1.isJanuary(), "jan1 mon");
assert(leap.isWeekday() && new Date(2024,1,25).isWeekend(), "weekday/weekend");
assert(new Date(2024,1,25).isSunday(), "isSunday");
/* all months + days round-trip via getWeekday/getMonth */
for (let m=0;m<12;m++){ const d=new Date(2024,m,15);
  const mp=["isJanuary","isFebruary","isMarch","isApril","isMay","isJune","isJuly","isAugust","isSeptember","isOctober","isNovember","isDecember"][m];
  assert(d[mp](), "month pred "+m); }

/* query */
eq(leap.getWeekday(), 4, "getWeekday Thu");
eq(leap.daysInMonth(), 29, "daysInMonth leap Feb");
eq(new Date(2023,1,1).daysInMonth(), 28, "daysInMonth non-leap Feb");
eq(new Date(2024,3,1).daysInMonth(), 30, "daysInMonth Apr");
eq(new Date(2024,0,1).getISOWeek(), 1, "ISO week Jan 1 2024");
eq(new Date(2024,11,31).getISOWeek(), 1, "ISO week Dec 31 2024 -> wk1 of 2025");
eq(new Date(2021,0,1).getISOWeek(), 53, "ISO week Jan 1 2021 -> wk53 of 2020");

/* add (immutable, JS field-overflow) */
eq(leap.addDays(1).getMonth(), 2, "addDays crosses month");
eq(leap.addDays(1).getDate(), 1, "addDays -> Mar 1");
eq(new Date(2024,0,31).addMonths(1).getMonth(), 2, "addMonths overflow Feb->Mar");
eq(new Date(2024,0,15).addWeeks(2).getDate(), 29, "addWeeks");
eq(new Date(2024,5,15).addHours(24).getDate(), 16, "addHours");
{ const before=jan1.getTime(); jan1.addDays(5); eq(jan1.getTime(), before, "add is immutable"); }

/* boundaries */
eq(leap.beginningOfDay().getHours(), 0, "beginningOfDay h");
eq(leap.beginningOfDay().getMinutes(), 0, "beginningOfDay m");
eq(leap.endOfDay().getHours(), 23, "endOfDay h");
eq(leap.endOfDay().getMilliseconds(), 999, "endOfDay ms");
eq(leap.beginningOfMonth().getDate(), 1, "beginningOfMonth");
eq(leap.endOfMonth().getDate(), 29, "endOfMonth leap");
eq(new Date(2023,1,10).endOfMonth().getDate(), 28, "endOfMonth non-leap");
eq(leap.beginningOfYear().getMonth(), 0, "beginningOfYear month");
eq(leap.beginningOfYear().getDate(), 1, "beginningOfYear date");
eq(leap.endOfYear().getMonth(), 11, "endOfYear month");
eq(leap.endOfYear().getDate(), 31, "endOfYear date");
eq(leap.beginningOfWeek().getDay(), 0, "beginningOfWeek = Sunday");
eq(leap.endOfWeek().getDay(), 6, "endOfWeek = Saturday");

/* compare */
assert(jan1.isBefore(jan11) && jan11.isAfter(jan1), "isBefore/After");
assert(!jan1.isAfter(jan11), "isAfter false");
assert(new Date(2024,0,5).isBetween(jan1,jan11), "isBetween in");
assert(!new Date(2024,0,20).isBetween(jan1,jan11), "isBetween out");
assert(new Date(2024,0,5).isBetween(jan11,jan1), "isBetween swapped bounds");

/* diffs */
eq(jan1.daysUntil(jan11), 10, "daysUntil");
eq(jan11.daysSince(jan1), 10, "daysSince");
eq(jan1.hoursUntil(jan11), 240, "hoursUntil");
eq(jan1.weeksUntil(new Date(2024,0,22)), 3, "weeksUntil");
eq(jan1.monthsUntil(new Date(2024,3,1)), 3, "monthsUntil");
eq(jan1.monthsUntil(new Date(2024,2,15)), 2, "monthsUntil partial (day not reached)");
eq(jan1.yearsUntil(new Date(2027,0,1)), 3, "yearsUntil");
eq(jan11.daysSince(jan1) + jan1.daysUntil(jan11), 20, "since/until symmetry");
assert(isNaN(new Date(NaN).daysUntil(jan1)), "diff of invalid = NaN");

/* advance / rewind (immutable) */
eq(jan1.advance({months:1,days:5}).getMonth(), 1, "advance months");
eq(jan1.advance({months:1,days:5}).getDate(), 6, "advance days");
eq(jan1.rewind({days:1}).getFullYear(), 2023, "rewind crosses year");
eq(jan1.advance({years:1,hours:-1}).getFullYear(), 2024, "advance mixed sign hours"); // Jan1 -1h stays 2024? actually Jan 1 2025 -1h = Dec 31 2024
{ const t=jan1.getTime(); jan1.advance({days:100}); eq(jan1.getTime(),t,"advance immutable"); }

/* clone */
{ const c=jan1.clone(); assert(c!==jan1 && c.getTime()===jan1.getTime(), "clone"); }

/* format */
eq(leap.format("{yyyy}-{MM}-{dd} {HH}:{mm}:{ss}"), "2024-02-29 15:30:45", "format numeric");
eq(leap.format("{Weekday}, {Month} {d}, {yyyy}"), "Thursday, February 29, 2024", "format names");
eq(leap.format("{h}{tt} {dow} {Mon} {SSS}"), "3pm Thu Feb 500", "format 12h+abbr+ms");
eq(leap.format("{yy}"), "24", "format 2-digit year");
eq(new Date(2024,0,5,9).format("{HH}:{hh} {H}:{h}"), "09:09 9:9", "format hours");
eq(jan1.format(), "2024-01-01 00:00:00", "format default");
eq(leap.format("plain text"), "plain text", "format no tokens");

/* iso */
assert(/^\d{4}-\d{2}-\d{2}T/.test(jan1.iso()), "iso format");
eq(jan1.iso(), jan1.toISOString(), "iso == toISOString");

/* now-relative (deterministic constructions) */
assert(new Date().isToday(), "isToday now");
assert(new Date(Date.now()+86400000).isTomorrow(), "isTomorrow");
assert(new Date(Date.now()-86400000).isYesterday(), "isYesterday");
assert(new Date(Date.now()+3600000).isFuture(), "isFuture");
assert(new Date(Date.now()-3600000).isPast(), "isPast");
assert(new Date(Date.now()-3600000).hoursAgo() === 1, "hoursAgo");
assert(new Date(Date.now()+86400000*2+2000).daysFromNow() === 2, "daysFromNow");
eq(new Date(Date.now()-3600000).relative(), "1 hour ago", "relative past");
eq(new Date(Date.now()+86400000*3+2000).relative(), "in 3 days", "relative future");

/* non-enumerable + invalid */
assert(Object.getOwnPropertyDescriptor(Date.prototype,"addDays").enumerable===false, "non-enum");
assert(new Date(NaN).isToday()===false && new Date(NaN).isValid()===false, "invalid preds");
eq(new Date(NaN).format("{yyyy}"), "Invalid Date", "invalid format");
eq(new Date(NaN).relative(), "Invalid Date", "invalid relative");
assert(isNaN(new Date(NaN).addDays(1).getTime()), "add to invalid = invalid");

console.log("test_date_ext.js OK — "+n+" assertions");
