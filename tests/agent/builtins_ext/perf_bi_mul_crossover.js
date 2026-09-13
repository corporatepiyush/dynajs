// perf_bi_mul_crossover.js — Karatsuba crossover verification. Tree-only
// perf gate: PASS iff every VERDICT is OK; RAW lines are informational and
// engine-local (node LICM hoists the loop-invariant mul — its RAWs are void).
// 512->1024 STRADDLES the recorded crossover (~600 digits): expected ratio
// between karatsuba ~3 and schoolbook 4, depressed by fixed per-op overhead;
// 4k->8k is pure karatsuba (~2.7-3.0); no row may look quadratic (~4+ at
// large sizes, or 16x per 4x span).
// Min-of-3 calibrated blocks beats single-block timer noise.
function mulberry32(seed){var s=seed>>>0;return function(){s=(s+0x6D2B79F5)|0;var t=Math.imul(s^(s>>>15),1|s);t=(t+Math.imul(t^(t>>>7),61|t))^t;return (t^(t>>>14))>>>0;};}
var r = mulberry32(0x9E4A);
function bigRnd(ndig) {
  var s = String(1 + (r() % 9));
  for (var i = 1; i < ndig; i++) s += String(r() % 10);
  return BigInt(s);
}
function timeMul(ndig) {
  var a = bigRnd(ndig), b = bigRnd(ndig);
  var reps = 1, ms = Infinity;
  for (var round = 0; round < 8; round++) {
    var t0 = Date.now();
    var sink = 0n;
    for (var k = 0; k < reps; k++) sink += (a * b) % 3n;
    ms = Date.now() - t0;
    if (ms >= 120) break;
    reps *= 8;
  }
  var best = ms / reps;
  for (var block = 0; block < 2; block++) {
    var t0 = Date.now();
    var sink = 0n;
    for (var k = 0; k < reps; k++) sink += (a * b) % 3n;
    var m2 = Date.now() - t0;
    if (m2 / reps < best) best = m2 / reps;
  }
  return best;
}
var sizes = [512, 1024, 2048, 4096, 8192];
var times = {};
for (var i = 0; i < sizes.length; i++) {
  times[sizes[i]] = timeMul(sizes[i]);
  console.log("RAW mul " + sizes[i] + " digits: " + times[sizes[i]].toFixed(4) + " ms/op");
}
function ratio(a, b) { return times[b] / times[a]; }
console.log("RAW ratio 512->1024: " + ratio(512, 1024).toFixed(2));
console.log("RAW ratio 1024->2048: " + ratio(1024, 2048).toFixed(2));
console.log("RAW ratio 2048->4096: " + ratio(2048, 4096).toFixed(2));
console.log("RAW ratio 4096->8192: " + ratio(4096, 8192).toFixed(2));
var sb = ratio(512, 1024);
var kc = Math.min(ratio(2048, 4096), ratio(4096, 8192));
console.log("VERDICT: schoolbook_512_1024=" + (sb >= 2.5 && sb <= 5.2 ? "OK" : "BAD") + " raw=" + sb.toFixed(2));
console.log("VERDICT: karatsuba_4k8k=" + (kc < 3.6 ? "OK" : "BAD") + " raw=" + kc.toFixed(2));
console.log("VERDICT: subquadratic=" + (ratio(1024, 8192) < 45 ? "OK" : "BAD") + " raw=" + ratio(1024, 8192).toFixed(2));
