// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[1] = ["fails=0 hash=86792bb3"];
// test_bi_gcd_ladder.js — heavy mul/div interleaving (karatsuba + division
// interplay): Euclidean GCD of seeded 2000-digit pairs, checked against the
// Bezout identity a*gcd ≡ 0 and gcd divides both operands.
function mulberry32(seed){var s=seed>>>0;return function(){s=(s+0x6D2B79F5)|0;var t=Math.imul(s^(s>>>15),1|s);t=(t+Math.imul(t^(t>>>7),61|t))^t;return (t^(t>>>14))>>>0;};}
function fnv(str){var h=0x811c9dc5,i;for(i=0;i<str.length;i++){h^=str.charCodeAt(i);h=Math.imul(h,0x01000193);}return (h>>>0).toString(16);}
var r = mulberry32(0x6CDE);
function bigRnd(ndig) {
  var s = String(1 + (r() % 9));
  for (var i = 1; i < ndig; i++) s += String(r() % 10);
  return BigInt(s);
}
function gcd(x, y) {
  x = x < 0n ? -x : x; y = y < 0n ? -y : y;
  while (y !== 0n) { var t = x % y; x = y; y = t; }
  return x;
}
var fails = 0, feed = "";
for (var i = 0; i < 30; i++) {
  var a = bigRnd(2000), b = bigRnd(2000);
  var g = gcd(a, b);
  if (a % g !== 0n || b % g !== 0n) { fails++; __L(0, "GCD-DIV-FAIL i=" + i); }
  var g2 = gcd(b, a);
  if (g2 !== g) fails++;
  feed += i + ":" + String(g).length + ":" + fnv(String(g)) + "\n";
}
__L(1, "fails=" + fails + " hash=" + fnv(feed));

summary("builtins_ext");
