// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["[WALL][LMT] ROW 0:5,30,0,-330,1730592000000", "[WALL][LMT] ROW 111:3,51,51,-330,1730586111221", "[WALL][LMT] ROW 222:23,36,41,-330,1730570801431", "[WALL][LMT] ROW 333:9,33,33,-330,1730606613663", "[WALL][LMT] ROW 444:17,42,23,-330,1730635943873", "[WALL][LMT] ROW 555:15,15,15,-330,1730540715105", "[WALL][LMT] ROW 666:11,48,5,-330,1730528285315", "[WALL][LMT] ROW 777:21,57,57,-330,1730564877547", "[WALL][LMT] ROW 888:5,54,47,-330,1730507087757", "[WALL][LMT] ROW 999:3,39,39,-330,1730498979989"];
__EXP[3] = ["SUMMARY mism=0 hashSafe=811c9dc5"];
__EXP[4] = ["[WALL][LMT] hashRisky=61963f6f"];
// test_date_tz_memo_2slot_thrash.js — the audit's killer pattern: alternate
// local-wall-keyed setters (setHours) with UTC-keyed ops (setUTCHours/setTime)
// 1000x on the same object near the NY fall-back, plus a two-object ping-pong.
// Every step verified against a fresh twin.
// Tagging: rows where the local-setter UTC conversion is transition-hint
// sensitive (spec-literal LocalTZA(dt) vs V8 pre-call heuristic — documented
// impl-defined class, dynajs is the spec-literal one) go to the [WALL] bucket,
// sticky once triggered (results may cascade); pre-1972 local years to [LMT].
// feedSafe must be byte-identical vs node AND baseline.
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
// dt = exact target wall (from pre-call fields + setter args); engines
// provably agree iff the two hint interpretations coincide.
function setterAmbiguous(dt, preT) {
  if (dt !== dt || preT !== preT) return true;
  var tA = dt + new Date(dt).getTimezoneOffset() * 60000;   // spec-literal hint
  var tB = dt + new Date(preT).getTimezoneOffset() * 60000; // V8 pre-call hint
  return tA !== tB;
}
var r = mulberry32(0x7E2A11);
var d = new Date(Date.UTC(2024, 10, 3, 5, 30, 0)); // inside NY fall-back hour
var feed = "", mism = 0, feedSafe = "", feedRisky = "", sticky = false;
for (var i = 0; i < 1000; i++) {
  var preT = d.getTime();
  var py = d.getFullYear(), pmo = d.getMonth(), pd = d.getDate();
  var dt = NaN;
  if (i % 2 === 0) d.setUTCHours((i * 7) % 24, (i * 3) % 60);
  else { var hh = (i * 5) % 24; d.setHours(hh, i % 60, i % 60, (i * 11) % 1000); dt = Date.UTC(py, pmo, pd, hh, i % 60, i % 60, (i * 11) % 1000); }
  var f = new Date(d.getTime());
  var a = d.getHours() + "," + d.getMinutes() + "," + d.getSeconds() + "," + d.getTimezoneOffset() + "," + d.getTime();
  var b = f.getHours() + "," + f.getMinutes() + "," + f.getSeconds() + "," + f.getTimezoneOffset() + "," + f.getTime();
  var line = i + ":" + a;
  var pf = { y: d.getFullYear(), mo: d.getMonth(), d: d.getDate(), h: d.getHours(), mi: d.getMinutes(), s: d.getSeconds(), ms: d.getMilliseconds() };
  if (pf.y < 1972 || wallRisky(pf.y, pf.mo, pf.d, pf.h, pf.mi, pf.s, pf.ms) || setterAmbiguous(dt, preT)) sticky = true;
  if (sticky) {
    feedRisky += line + "\n";
    if (i % 111 === 0) __L(0, "[WALL][LMT] ROW " + line);
  } else {
    feedSafe += line + "\n";
    if (i % 111 === 0) __L(1, "ROW " + line);
  }
  feed += line + "\n";
  if (a !== b) { mism++; if (mism < 6) __L(2, "MISMATCH i=" + i + " " + a + " vs " + b); }
}
// two-object ping-pong: UTC mutations on one, local on the other, cross-read
var x = new Date(Date.UTC(2024, 2, 9, 12, 0, 0));
var y = new Date(Date.UTC(2024, 2, 9, 12, 0, 0));
for (var i = 0; i < 500; i++) {
  var preY = y.getTime();
  var pyy = y.getFullYear(), pmm = y.getMonth(), pdd = y.getDate(), phh = y.getHours(), pmii = y.getMinutes();
  x.setUTCMinutes(x.getUTCMinutes() + 30);
  y.setMinutes(pmii + 30);
  var dtY = Date.UTC(pyy, pmm, pdd, phh, pmii + 30, y.getSeconds(), y.getMilliseconds());
  var fx = new Date(x.getTime()), fy = new Date(y.getTime());
  var lx = x.getTime() + ":" + x.getHours() + ":" + fx.getHours();
  var ly = y.getTime() + ":" + y.getHours() + ":" + fy.getHours();
  var line = lx + "|" + ly;
  feed += line + "\n";
  var pf = { y: y.getFullYear(), mo: y.getMonth(), d: y.getDate(), h: y.getHours(), mi: y.getMinutes(), s: y.getSeconds(), ms: y.getMilliseconds() };
  if (sticky || pf.y < 1972 || wallRisky(pf.y, pf.mo, pf.d, pf.h, pf.mi, pf.s, pf.ms) || setterAmbiguous(dtY, preY)) { sticky = true; feedRisky += line + "\n"; }
  else feedSafe += line + "\n";
  if (x.getHours() !== fx.getHours() || y.getHours() !== fy.getHours()) mism++;
}
__L(3, "SUMMARY mism=" + mism + " hashSafe=" + fnv(feedSafe));
__L(4, "[WALL][LMT] hashRisky=" + fnv(feedRisky));

summary("builtins_ext");
