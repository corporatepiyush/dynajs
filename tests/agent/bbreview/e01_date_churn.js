// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[1] = ["g1 2023 10 15 3 43 20 123"];
__EXP[2] = ["g2 2023 10 15 3 43 20 124"];
__EXP[6] = [["~t1 1700000000123", "~t1 1700000000123"]];
// E: two Dates <1ms apart, alternating getters 10k times (cache churn)
const d1 = new Date(1700000000123);
const d2 = new Date(1700000000124);
let s = 0;
for (let i = 0; i < 10000; i++) {
  const d = i % 2 ? d1 : d2;
  s += d.getFullYear() + d.getMonth() + d.getDate() + d.getHours() + d.getMinutes() + d.getSeconds() + d.getMilliseconds();
}
__A("e01_date_churn.js:churn-sum", function () { assert_eq(s, 22375000, "churn-sum"); });
__L(1, "g1", d1.getFullYear(), d1.getMonth(), d1.getDate(), d1.getHours(), d1.getMinutes(), d1.getSeconds(), d1.getMilliseconds());
__L(2, "g2", d2.getFullYear(), d2.getMonth(), d2.getDate(), d2.getHours(), d2.getMinutes(), d2.getSeconds(), d2.getMilliseconds());
__A("e01_date_churn.js:churn-diff", function () { assert_eq(d2.getTime() - d1.getTime(), 1, "churn-diff"); });
const d3 = new Date(0);
let s2 = 0;
for (let i = 0; i < 10000; i++) {
  d3.setTime(i % 2 ? 1700000000123.4 : 1700000000123.9);
  s2 += d3.getMilliseconds() + d3.getSeconds();
}
__A("e01_date_churn.js:subms", function () { assert_eq(s2, 1430000, "subms"); });
__A("e01_date_churn.js:iso", function () { assert_eq(d1.toISOString(), "2023-11-14T22:13:20.123Z", "iso"); });
__L(6, "t1", d1.getTime(), d2.getTime(), d3.getTime());

summary("bbreview");
