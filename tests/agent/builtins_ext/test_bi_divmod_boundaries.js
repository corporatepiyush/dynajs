// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[2] = ["fails=0 hash=232a6386"];
// test_bi_divmod_boundaries.js — division/mod by 1-word and near-word
// divisors on big values (Burnikel/Zimmermann vs schoolbook boundary class):
// identity q*b + r === a for 200 seeded pairs incl. divisors 1..2^67.
function mulberry32(seed){var s=seed>>>0;return function(){s=(s+0x6D2B79F5)|0;var t=Math.imul(s^(s>>>15),1|s);t=(t+Math.imul(t^(t>>>7),61|t))^t;return (t^(t>>>14))>>>0;};}
function fnv(str){var h=0x811c9dc5,i;for(i=0;i<str.length;i++){h^=str.charCodeAt(i);h=Math.imul(h,0x01000193);}return (h>>>0).toString(16);}
var r = mulberry32(0xD177);
function bigRnd(ndig) {
  var s = String(1 + (r() % 9));
  for (var i = 1; i < ndig; i++) s += String(r() % 10);
  return BigInt(s);
}
var fails = 0, feed = "";
for (var i = 0; i < 200; i++) {
  var a = bigRnd(20 + (r() % 2000));
  var b;
  var pick = r() % 5;
  if (pick === 0) b = BigInt(1 + (r() % 4294967295));           // 1 word
  else if (pick === 1) b = 2n ** BigInt(1 + (r() % 67));         // power of two
  else if (pick === 2) b = bigRnd(1 + (r() % 5));                // tiny
  else if (pick === 3) b = bigRnd(10 + (r() % 1000));            // medium
  else b = (2n ** 64n - 1n) * (2n ** BigInt(r() % 30));          // multi-limb dense
  if (b === 0n) continue;
  var q = a / b, rem = a % b;
  if (q * b + rem !== a) { fails++; if (fails < 5) __L(0, "DIVMOD-FAIL i=" + i); }
  if (rem < 0n || rem >= (b < 0n ? -b : b)) { fails++; __L(1, "REM-RANGE-FAIL i=" + i); }
  feed += String(a).length + ":" + String(b).length + ":" + String(q).length + "\n";
}
// sign conventions (truncated division)
if (7n / -2n !== -3n || -7n / 2n !== -3n || 7n % -2n !== 1n || -7n % 2n !== -1n) fails++;
__L(2, "fails=" + fails + " hash=" + fnv(feed));

summary("builtins_ext");
