// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[2] = ["fails=0 nonzero=0 acc=524664"];
// test_lc_equal_fastpath.js — equal-string fast path: self-compare must be 0
// for ASCII short/long, wide short/long, mixed; 50k seeded self-compares
// must ALL be 0 (fast path may not corrupt the sign/value).
function mulberry32(seed){var s=seed>>>0;return function(){s=(s+0x6D2B79F5)|0;var t=Math.imul(s^(s>>>15),1|s);t=(t+Math.imul(t^(t>>>7),61|t))^t;return (t^(t>>>14))>>>0;};}
var r = mulberry32(0x10C8A5);
var fails = 0;
var samples = [
  "", "a", "abc", "The quick brown fox",
  "\u00e9", "\u00e9\u00e8\u00ea", "\u4e16\u754c", "a\u00e9\u4e16z",
  "\u007f\u0080\u00ff", "x".repeat(200), "\u4e16".repeat(200),
  "e\u0301", "\u00e9", "a\u0328\u0301", "\u0105"
];
for (var i = 0; i < samples.length; i++) {
  if (samples[i].localeCompare(samples[i]) !== 0) { fails++; __L(0, "SELF-NONZERO i=" + i); }
}
// equal-by-construction pairs
for (var i = 0; i < samples.length; i++) {
  for (var j = 0; j < samples.length; j++) {
    if (samples[i] === samples[j] && samples[i].localeCompare(samples[j]) !== 0) {
      fails++; __L(1, "PAIR-NONZERO " + i + "," + j);
    }
  }
}
// 50k seeded self-compares (wide strings exercise the fast path the audit measured)
var nonzero = 0, acc = 0;
for (var i = 0; i < 50000; i++) {
  var s = "";
  var n = 1 + (r() % 20);
  for (var k = 0; k < n; k++) s += String.fromCharCode(0x4e00 + (r() % 5000));
  var v = s.localeCompare(s);
  if (v !== 0) nonzero++;
  acc = (acc + v + n) % 1000000007;
}
__L(2, "fails=" + fails + " nonzero=" + nonzero + " acc=" + acc);

summary("builtins_ext");
