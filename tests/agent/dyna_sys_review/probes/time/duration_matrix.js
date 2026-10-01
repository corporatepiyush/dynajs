import { Duration, parseDuration, durationString } from "dyna:time";
import { ok, eq, setTag, done } from "../../rh_module.js";
setTag("time.duration_matrix");

var d0 = new Duration({ months: 0 });
eq(d0.years, 0, "months=0 years==0");
eq(d0.months, 0, "months=0 months==0");

var d1 = new Duration({ months: 1 });
eq(d1.years, 0, "months=1 years==0");
eq(d1.months, 1, "months=1 months==1");

var d12 = new Duration({ months: 12 });
eq(d12.years, 1, "months=12 years==1");
eq(d12.months, 0, "months=12 months==0");
eq(String(d12), "P1Y", "months=12 string P1Y, got " + String(d12));

var d14 = new Duration({ months: 14, days: 3 });
eq(d14.years, 1, "months=14 years==1");
eq(d14.months, 2, "months=14 months==2 (remainder)");
eq(d14.days, 3, "months=14 days preserved");
eq(String(d14), "P1Y2M3D", "months=14 string P1Y2M3D");

var d25 = new Duration({ months: 25 });
eq(d25.years, 2, "months=25 years==2");
eq(d25.months, 1, "months=25 months==1");

var dn = new Duration({ months: -14 });
eq(dn.years, -1, "months=-14 years==-1, got " + dn.years);
eq(dn.months, -2, "months=-14 months==-2, got " + dn.months);

var dy = new Duration({ years: 2, months: 3 });
eq(dy.years, 2, "years=2,months=3 years==2");
eq(dy.months, 3, "years=2,months=3 months==3");
eq(String(dy), "P2Y3M", "years=2,months=3 string P2Y3M");

var combos = [0, 1, 11, 12, 14, 23, 25, 36, -14, -25];
for (var i = 0; i < combos.length; i++) {
  var d = new Duration({ months: combos[i] });
  eq(d.years * 12 + d.months, combos[i],
     "round-trip months=" + combos[i] + " -> " + d.years + "*12+" + d.months);
}

var dm = new Duration({ years: 1, months: 14 });
eq(dm.years * 12 + dm.months, 26, "years=1,months=14 total 26");

var threw = null, p14 = null;
try { p14 = parseDuration("P14M"); } catch (e) { threw = e; }
ok(threw !== null || p14 !== undefined, "parseDuration P14M parses or documents a refusal");
var d14b = new Duration({ months: 14 });
ok(durationString(d14b) === "P1Y2M" || String(d14b) === "P1Y2M",
   "durationString(months=14) P1Y2M, got " + (durationString(d14b) || String(d14b)));

setTag("time.duration_matrix");
done();
