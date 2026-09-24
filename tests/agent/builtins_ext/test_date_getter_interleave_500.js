// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["[WALL][LMT] ROW 89:2817968807915|2059|3|19|6|14|36|47|915|2059|3|19|6|9|6|47|915|-330|2059-04-19T09:06:47.915Z", "[WALL][LMT] ROW 178:2776725547283532|89960|11|6|2|9|31|23|532|89960|11|6|2|4|1|23|532|-330|+089960-12-06T04:01:23.532Z", "[WALL][LMT] ROW 267:4262554180325|2105|0|28|3|8|19|40|325|2105|0|28|3|2|49|40|325|-330|2105-01-28T02:49:40.325Z", "[WALL][LMT] ROW 356:1281981132800|2010|7|16|1|23|22|12|800|2010|7|16|1|17|52|12|800|-330|2010-08-16T17:52:12.800Z", "[WALL][LMT] ROW 445:2841389456070|2060|0|15|4|16|20|56|70|2060|0|15|4|10|50|56|70|-330|2060-01-15T10:50:56.070Z"];
__EXP[1] = ["ROW 0:1675728000000|2023|1|7|2|5|30|0|0|2023|1|7|2|0|0|0|0|-330|2023-02-07T00:00:00.000Z"];
__EXP[3] = ["SUMMARY mism=0 hashSafe=8fa3a273"];
__EXP[4] = [["[WALL][LMT] hashRisky=e2fb7c4e", "[WALL][LMT] hashRisky=d8ac03aa"]];
__REQ = {"dynajs": {"0": 5, "1": 1, "3": 1, "4": 1}, "node": {"0": 5, "1": 1, "3": 1, "4": 1}};
// test_date_getter_interleave_500.js — 500 seeded getter/setter interleavings
// on ONE object (every local + UTC getter, every local + UTC setter) with a
// fresh-twin equality check after every step. The per-object breakdown cache
// must be indistinguishable from no cache at all.
function mulberry32(seed){var s=seed>>>0;return function(){s=(s+0x6D2B79F5)|0;var t=Math.imul(s^(s>>>15),1|s);t=(t+Math.imul(t^(t>>>7),61|t))^t;return (t^(t>>>14))>>>0;};}
function fnv(str){var h=0x811c9dc5,i;for(i=0;i<str.length;i++){h^=str.charCodeAt(i);h=Math.imul(h,0x01000193);}return (h>>>0).toString(16);}
function wallRisky(y, mo, d, h, mi, s, ms) {
  var base = Date.UTC(y, mo, d, h, mi, s, ms);
  if (base !== base) return true;
  var o1 = new Date(base).getTimezoneOffset();
  var t1 = base - o1 * 60000;
  var o2 = new Date(t1).getTimezoneOffset();
  return o1 !== o2;
}
function setterAmbiguous(dt, preT) {
  if (dt !== dt || preT !== preT) return true;
  var tA = dt + new Date(dt).getTimezoneOffset() * 60000;   // spec-literal hint
  var tB = dt + new Date(preT).getTimezoneOffset() * 60000; // V8 pre-call hint
  return tA !== tB;
}
var r = mulberry32(0xABCD01);
var d = new Date(Date.UTC(2023, 6, 1, 0, 0, 0));
var feed = "", mism = 0, feedSafe = "", feedRisky = "", sticky = false;
function all(x) {
  return [x.getTime(), x.getFullYear(), x.getMonth(), x.getDate(), x.getDay(),
          x.getHours(), x.getMinutes(), x.getSeconds(), x.getMilliseconds(),
          x.getUTCFullYear(), x.getUTCMonth(), x.getUTCDate(), x.getUTCDay(),
          x.getUTCHours(), x.getUTCMinutes(), x.getUTCSeconds(), x.getUTCMilliseconds(),
          x.getTimezoneOffset(), x.toISOString()].join("|");
}
for (var i = 0; i < 500; i++) {
  var preT = d.getTime();
  var py = d.getFullYear(), pmo = d.getMonth(), pd = d.getDate();
  var ph = d.getHours(), pmi = d.getMinutes(), ps = d.getSeconds(), pms = d.getMilliseconds();
  var op = r() % 14;
  var dt = NaN; // exact target wall for local setters (NaN for UTC/time setters)
  switch (op) {
    case 0: var y0 = 1970 + (r() % 200), m0 = r() % 12, dd0 = (r() % 28) + 1; d.setFullYear(y0, m0, dd0); dt = Date.UTC(y0, m0, dd0, ph, pmi, ps, pms); break;
    case 1: var m1 = r() % 12, d1 = (r() % 28) + 1; d.setMonth(m1, d1); dt = Date.UTC(py, m1, d1, ph, pmi, ps, pms); break;
    case 2: var d2 = (r() % 28) + 1; d.setDate(d2); dt = Date.UTC(py, pmo, d2, ph, pmi, ps, pms); break;
    case 3: var h3 = r() % 24, mi3 = r() % 60, s3 = r() % 60, ms3 = r() % 1000; d.setHours(h3, mi3, s3, ms3); dt = Date.UTC(py, pmo, pd, h3, mi3, s3, ms3); break;
    case 4: var mi4 = r() % 60, s4 = r() % 60, ms4 = r() % 1000; d.setMinutes(mi4, s4, ms4); dt = Date.UTC(py, pmo, pd, ph, mi4, s4, ms4); break;
    case 5: var s5 = r() % 60, ms5 = r() % 1000; d.setSeconds(s5, ms5); dt = Date.UTC(py, pmo, pd, ph, pmi, s5, ms5); break;
    case 6: var ms6 = r() % 1000; d.setMilliseconds(ms6); dt = Date.UTC(py, pmo, pd, ph, pmi, ps, ms6); break;
    case 7: d.setUTCFullYear(1970 + (r() % 200), r() % 12, (r() % 28) + 1); break;
    case 8: d.setUTCMonth(r() % 12, (r() % 28) + 1); break;
    case 9: d.setUTCDate((r() % 28) + 1); break;
    case 10: d.setUTCHours(r() % 24, r() % 60, r() % 60, r() % 1000); break;
    case 11: d.setUTCMinutes(r() % 60, r() % 60, r() % 1000); break;
    case 12: d.setUTCSeconds(r() % 60, r() % 1000); break;
    default: d.setTime(Math.floor((r() / 4294967296 * 2 - 1) * 8.64e15));
  }
  var f = new Date(d.getTime());
  var l1 = all(d), l2 = all(f);
  var line = i + ":" + l1;
  var pf = { y: d.getFullYear(), mo: d.getMonth(), d: d.getDate(), h: d.getHours(), mi: d.getMinutes(), s: d.getSeconds(), ms: d.getMilliseconds() };
  if (pf.y < 1972 || wallRisky(pf.y, pf.mo, pf.d, pf.h, pf.mi, pf.s, pf.ms) || setterAmbiguous(dt, preT)) sticky = true;
  if (sticky) {
    feedRisky += line + "\n";
    if (i % 89 === 0) __L(0, "[WALL][LMT] ROW " + line);
  } else {
    feedSafe += line + "\n";
    if (i % 89 === 0) __L(1, "ROW " + line);
  }
  feed += line + "\n";
  if (l1 !== l2) { mism++; if (mism < 6) __L(2, "MISMATCH i=" + i + " op=" + op + "\n  cached=" + l1 + "\n  fresh =" + l2); }
}
__L(3, "SUMMARY mism=" + mism + " hashSafe=" + fnv(feedSafe));
__L(4, "[WALL][LMT] hashRisky=" + fnv(feedRisky));

summary("builtins_ext");
