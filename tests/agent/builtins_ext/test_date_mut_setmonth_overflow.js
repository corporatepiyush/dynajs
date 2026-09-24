// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[1] = ["ROW 0:2022,6,20,17,30,0,0,3:1658318400000", "ROW 97:2019,10,22,19,18,51,761,5:1574430531761", "ROW 194:2019,2,8,21,4,54,836,5:1552059294836", "ROW 291:2021,4,9,1,16,14,446,0:1620503174446", "ROW 388:2021,10,25,20,55,41,360,4:1637853941360", "ROW 485:2022,1,11,12,18,28,334,5:1644562108334"];
__EXP[3] = ["SUMMARY mism=0 hashSafe=33d71288"];
__EXP[4] = ["[WALL][LMT] hashRisky=811c9dc5"];
// test_date_mut_setmonth_overflow.js — 500 seeded mutation sequences through
// setMonth overflow chains (Jan31.setMonth(1)->Mar2/3, setMonth(12), day-31
// clamps), setFullYear on leap boundaries, setDate/setMinutes carry chains.
// After EVERY mutation: cached getters must equal a fresh twin's getters.
function mulberry32(seed){var s=seed>>>0;return function(){s=(s+0x6D2B79F5)|0;var t=Math.imul(s^(s>>>15),1|s);t=(t+Math.imul(t^(t>>>7),61|t))^t;return (t^(t>>>14))>>>0;};}
function fnv(str){var h=0x811c9dc5,i;for(i=0;i<str.length;i++){h^=str.charCodeAt(i);h=Math.imul(h,0x01000193);}return (h>>>0).toString(16);}
// wallRisky: the local wall fields reachable from t sit in a DST gap/fold
// (two different offsets reachable from the same wall clock). Engine-independent.
function wallRisky(y, mo, d, h, mi, s, ms) {
  var base = Date.UTC(y, mo, d, h, mi, s, ms);
  if (base !== base) return true;
  var o1 = new Date(base).getTimezoneOffset();
  var t1 = base - o1 * 60000;
  var o2 = new Date(t1).getTimezoneOffset();
  return o1 !== o2;
}
// setterAmbiguous: the local-setter UTC conversion is transition-hint-sensitive
// (documented impl-defined class: spec-literal LocalTZA(dt) hint vs V8's
// pre-call-instant heuristic). dt = exact target wall derived from pre-call
// fields + setter args. Engines provably agree iff tA === tB.
function setterAmbiguous(dt, preT) {
  if (dt !== dt || preT !== preT) return true;
  var tA = dt + new Date(dt).getTimezoneOffset() * 60000;   // spec-literal hint
  var tB = dt + new Date(preT).getTimezoneOffset() * 60000; // V8 pre-call hint
  return tA !== tB;
}
var r = mulberry32(0x500A1);
var d = new Date(Date.UTC(2024, 0, 31, 12, 0, 0)); // Jan 31 2024
var feed = "", mism = 0, feedSafe = "", feedRisky = "", sticky = false;
function g8(x) {
  return x.getFullYear() + "," + x.getMonth() + "," + x.getDate() + "," +
         x.getHours() + "," + x.getMinutes() + "," + x.getSeconds() + "," +
         x.getMilliseconds() + "," + x.getDay();
}
for (var i = 0; i < 500; i++) {
  var preT = d.getTime();
  var py = d.getFullYear(), pmo = d.getMonth(), pd = d.getDate();
  var ph = d.getHours(), pmi = d.getMinutes(), ps = d.getSeconds(), pms = d.getMilliseconds();
  var op = r() % 6;
  var dt; // exact target wall (for the ambiguity classification)
  if (op === 0) { var dm = (r() % 25) - 12; d.setMonth(pmo + dm); dt = Date.UTC(py, pmo + dm, pd, ph, pmi, ps, pms); } // overflow chains
  else if (op === 1) { var m1 = r() % 13, d1 = r() % 32; d.setMonth(m1, d1); dt = Date.UTC(py, m1, d1, ph, pmi, ps, pms); }
  else if (op === 2) { var y2 = 2019 + (r() % 9), m2 = r() % 13, d2 = r() % 32; d.setFullYear(y2, m2, d2); dt = Date.UTC(y2, m2, d2, ph, pmi, ps, pms); } // leap boundaries
  else if (op === 3) { var dd = (r() % 60) - 30; d.setDate(pd + dd); dt = Date.UTC(py, pmo, pd + dd, ph, pmi, ps, pms); }
  else if (op === 4) { var h4 = r() % 25, mi4 = r() % 60, s4 = r() % 62, ms4 = r() % 1000; d.setHours(h4, mi4, s4, ms4); dt = Date.UTC(py, pmo, pd, h4, mi4, s4, ms4); } // overflow args
  else { var dmm = (r() % 90) - 45; d.setMinutes(pmi + dmm); dt = Date.UTC(py, pmo, pd, ph, pmi + dmm, ps, pms); }
  var t = d.getTime();
  var f = new Date(t); // cache-cold twin
  var l1 = g8(d), l2 = g8(f);
  var line = i + ":" + l1 + ":" + t;
  var pf = { y: d.getFullYear(), mo: d.getMonth(), d: d.getDate(), h: d.getHours(), mi: d.getMinutes(), s: d.getSeconds(), ms: d.getMilliseconds() };
  if (pf.y < 1972 || wallRisky(pf.y, pf.mo, pf.d, pf.h, pf.mi, pf.s, pf.ms) || setterAmbiguous(dt, preT)) sticky = true;
  if (sticky) {
    feedRisky += line + "\n";
    if (i % 97 === 0) __L(0, "[WALL][LMT] ROW " + line);
  } else {
    feedSafe += line + "\n";
    if (i % 97 === 0) __L(1, "ROW " + line);
  }
  feed += line + "\n";
  if (l1 !== l2) { mism++; if (mism < 6) __L(2, "MISMATCH i=" + i + " " + l1 + " vs " + l2); }
}
__L(3, "SUMMARY mism=" + mism + " hashSafe=" + fnv(feedSafe));
__L(4, "[WALL][LMT] hashRisky=" + fnv(feedRisky));

summary("builtins_ext");
