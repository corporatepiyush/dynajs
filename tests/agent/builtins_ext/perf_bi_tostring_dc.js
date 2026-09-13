// perf_bi_tostring_dc.js — D&C toString(10) growth: quadratic would double
// ratio ~4 per size doubling; D&C lands ~2-3. VERDICT-mode file.
function mulberry32(seed){var s=seed>>>0;return function(){s=(s+0x6D2B79F5)|0;var t=Math.imul(s^(s>>>15),1|s);t=(t+Math.imul(t^(t>>>7),61|t))^t;return (t^(t>>>14))>>>0;};}
var r = mulberry32(0x77A0);
function bigRnd(ndig) {
  var s = String(1 + (r() % 9));
  for (var i = 1; i < ndig; i++) s += String(r() % 10);
  return BigInt(s);
}
var sizes = [1000, 2000, 4000, 8000, 16000];
var vals = [], times = {};
for (var i = 0; i < sizes.length; i++) vals.push(bigRnd(sizes[i]));
for (var i = 0; i < sizes.length; i++) {
  var reps = 1, ms = 0, out = "";
  for (var round = 0; round < 6; round++) {
    var t0 = Date.now();
    for (var k = 0; k < reps; k++) out = vals[i].toString(10);
    ms = Date.now() - t0;
    if (ms >= 40) break;
    reps *= 8;
  }
  times[sizes[i]] = ms / reps;
  console.log("RAW toString " + sizes[i] + " digits: " + times[sizes[i]].toFixed(4) + " ms len=" + out.length +
    " head=" + out.slice(0, 8) + " tail=" + out.slice(-8));
}
function ratio(a, b) { return times[b] / times[a]; }
console.log("RAW ratio 1000->2000: " + ratio(1000, 2000).toFixed(2));
console.log("RAW ratio 2000->4000: " + ratio(2000, 4000).toFixed(2));
console.log("RAW ratio 4000->8000: " + ratio(4000, 8000).toFixed(2));
console.log("RAW ratio 8000->16000: " + ratio(8000, 16000).toFixed(2));
var worst = Math.max(ratio(4000, 8000), ratio(8000, 16000));
console.log("RAW worst ratio " + worst.toFixed(2) + " (quadratic=4)");
console.log("VERDICT: dc_tostring=" + (worst < 3.8 ? "OK" : "BAD"));
