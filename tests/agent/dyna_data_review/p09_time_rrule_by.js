import { RRule } from "dyna:time";
import { eq as EQ, ok as OK, throws as TH, done as DONE } from "./harness.js";

function iso(t) { return t.getTime ? t.toISOString() : String(t); }
function times(r) { return r.all().map(iso); }

TH(function () { new RRule({ freq: "DAILY", byhour: [9] }); }, "SyntaxError", "byhour refused");
TH(function () { new RRule({ freq: "DAILY", byminute: [30] }); }, "SyntaxError", "byminute refused");
TH(function () { new RRule({ freq: "DAILY", bysecond: [0] }); }, "SyntaxError", "bysecond refused");
TH(function () { new RRule({ freq: "YEARLY", byeaster: 0 }); }, "SyntaxError", "byeaster refused");
TH(function () { new RRule({ freq: "DAILY", count: 5, byhour: [9, 17] }); }, "SyntaxError", "byhour among valid options refused");
TH(function () { RRule.fromString("FREQ=DAILY;BYHOUR=9"); }, "SyntaxError", "string BYHOUR refused");
new RRule({ freq: "DAILY", count: 2, byhour: null });
OK(true, "byhour:null accepted");
new RRule({ freq: "DAILY", count: 2 });
OK(true, "absent byhour accepted");

EQ(times(new RRule({ freq: "DAILY", count: 4, dtstart: "2026-01-01T00:00:00Z" })),
   ["2026-01-01T00:00:00.000Z","2026-01-02T00:00:00.000Z","2026-01-03T00:00:00.000Z","2026-01-04T00:00:00.000Z"],
   "DAILY count=4");
EQ(times(new RRule({ freq: "WEEKLY", byweekday: ["MO","WE"], count: 4, dtstart: "2026-01-07T00:00:00Z" })),
   ["2026-01-07T00:00:00.000Z","2026-01-12T00:00:00.000Z","2026-01-14T00:00:00.000Z","2026-01-19T00:00:00.000Z"],
   "WEEKLY MO,WE count=4");
EQ(times(new RRule({ freq: "MONTHLY", bymonthday: 1, count: 3, dtstart: "2026-01-01T00:00:00Z" })),
   ["2026-01-01T00:00:00.000Z","2026-02-01T00:00:00.000Z","2026-03-01T00:00:00.000Z"],
   "MONTHLY day1 count=3");
EQ(times(new RRule({ freq: "YEARLY", bymonth: 12, bymonthday: 25, count: 2, dtstart: "2026-02-10T00:00:00Z" })),
   ["2026-12-25T00:00:00.000Z","2027-12-25T00:00:00.000Z"],
   "YEARLY Dec25 count=2");
EQ(times(new RRule({ freq: "MONTHLY", bymonthday: -1, count: 3, dtstart: "2026-01-31T00:00:00Z" })),
   ["2026-01-31T00:00:00.000Z","2026-02-28T00:00:00.000Z","2026-03-31T00:00:00.000Z"],
   "MONTHLY last-day (Feb non-leap)");
var oc = times(new RRule({ freq: "WEEKLY", byweekday: ["MO","WE"], count: 4, dtstart: "2026-01-07T00:00:00Z" }));
var sc = times(RRule.fromString("FREQ=WEEKLY;BYDAY=MO,WE;COUNT=4", { dtstart: "2026-01-07T00:00:00Z" }));
EQ(oc, sc, "object form == string form");
DONE("p09_time_rrule_by");
