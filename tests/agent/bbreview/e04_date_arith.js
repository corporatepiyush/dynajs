// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[1] = [["v2 Wed Nov 15 2023 03:43:20 GMT+05301", "v2 Wed Nov 15 2023 03:43:20 GMT+0530 (India Standard Time)1"]];
__EXP[2] = [["~v3", "~v3"]];
__EXP[3] = [["~p1", "~p1"]];
__EXP[4] = [["~p2", "~p2"]];
__EXP[6] = ["p4 2024-02-29T12:00:00.000Z 29 4"];
__EXP[7] = ["r1 58 59 500"];
__EXP[8] = ["r2 56 59 500"];
__EXP[9] = ["r3 54 55"];
__EXP[10] = ["r4 19 14 0"];
__EXP[11] = [["~r5", "~r5"]];
__EXP[12] = [["~r6 16", "~r6 16"]];
__EXP[13] = [["~q", "~q"]];
__EXP[14] = [["~z", "~z"]];
__REQ = {"dynajs": {"1": 1, "2": 1, "3": 1, "4": 1, "6": 1, "7": 1, "8": 1, "9": 1, "10": 1, "11": 1, "12": 1, "13": 1, "14": 1}, "node": {"1": 1, "2": 1, "3": 1, "4": 1, "6": 1, "7": 1, "8": 1, "9": 1, "10": 1, "11": 1, "12": 1, "13": 1, "14": 1}};
// E: valueOf+arithmetic chains, Date.parse interplay, rollover chains
const d = new Date(1700000000000);
const x = d.valueOf() + 1000 * 60 * 60;
__A("e04_date_arith.js:v1", function () { assert_eq(new Date(x).toISOString(), "2023-11-14T23:13:20.000Z", "v1"); });
__L(1, "v2", d + 1);
__L(2, "v3", d * 1, d - 0);
__L(3, "p1", Date.parse("2023-11-14T22:13:20.000Z"), Date.parse("2023-11-14T22:13:20Z"));
__L(4, "p2", Date.parse("2023-11-14"), Date.parse("2023-11-14T22:13:20.123Z"));
__A("e04_date_arith.js:p3", function () { assert_eq(Date.parse("Wed, 15 Nov 2023 01:13:20 GMT"), 1700010800000, "p3"); });
const t = Date.parse("2024-02-29T12:00:00Z");
const dp = new Date(t);
__L(6, "p4", dp.toISOString(), dp.getDate(), dp.getDay());
const r = new Date(2024, 0, 15, 12, 0, 0, 0);
r.setMilliseconds(-1500);
__L(7, "r1", r.getSeconds(), r.getMinutes(), r.getMilliseconds());
r.setMilliseconds(-1500);
__L(8, "r2", r.getSeconds(), r.getMinutes(), r.getMilliseconds());
for (let i = 0; i < 5; i++) r.setSeconds(-1 - i);
__L(9, "r3", r.getMinutes(), r.getSeconds());
r.setHours(-5);
__L(10, "r4", r.getHours(), r.getDate(), r.getMonth());
r.setMonth(-13);
__L(11, "r5", r.getMonth(), r.getFullYear());
r.setDate(-45);
__L(12, "r6", r.getDate(), r.getMonth(), r.getFullYear());
const q = new Date(2024, 5, 1, 12, 30, 15, 500);
let s = 0;
for (let i = 0; i < 1000; i++) {
  q.setMilliseconds(400 + i);
  s += q.getSeconds() + q.getMilliseconds();
}
__L(13, "q", s, q.getTime());
// parse cache interplay: parse -> construct -> setTime -> getters
const t2 = Date.parse("1970-01-01T00:00:00Z");
const z = new Date(t2);
z.setTime(t2 + 86400000 * 365);
__L(14, "z", z.toISOString(), z.getFullYear());
__A("e04_date_arith.js:pc1", function () { assert_eq(Date.parse("2023-11-14T22:13:20Z") === Date.parse("2023-11-14T22:13:20.000Z"), true, "pc1"); });

summary("bbreview");
