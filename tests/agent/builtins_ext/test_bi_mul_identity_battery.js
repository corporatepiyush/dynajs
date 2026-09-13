// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["FAIL i=0 da=3893 db=748", "FAIL i=1 da=1835 db=2649", "FAIL i=3 da=2400 db=305", "FAIL i=4 da=963 db=1627"];
__EXP[1] = ["fails=160 hash=b7b139c2"];
// test_bi_mul_identity_battery.js — 200 seeded random mul pairs across sizes
// (10..4000 digits): (a*b)/a === b, (a*b)%a === 0n, sign and zero identities.
function mulberry32(seed){var s=seed>>>0;return function(){s=(s+0x6D2B79F5)|0;var t=Math.imul(s^(s>>>15),1|s);t=(t+Math.imul(t^(t>>>7),61|t))^t;return (t^(t>>>14))>>>0;};}
function fnv(str){var h=0x811c9dc5,i;for(i=0;i<str.length;i++){h^=str.charCodeAt(i);h=Math.imul(h,0x01000193);}return (h>>>0).toString(16);}
var r = mulberry32(0x1D3A);
function bigRnd(ndig) {
  var s = String(1 + (r() % 9));
  for (var i = 1; i < ndig; i++) s += String(r() % 10);
  return BigInt(s);
}
var fails = 0, feed = "";
for (var i = 0; i < 200; i++) {
  var da = 10 + (r() % 3990), db = 10 + (r() % 3990);
  var a = bigRnd(da), b = bigRnd(db);
  var p = a * b;
  var ok = (p / a === b) && (p % a === 0n) && String(p).length === da + db - 1 + (String(p).length === da + db ? 0 : 0);
  // digit-count bound instead of exact: p < 10^(da+db)
  var inRange = p < 10n ** BigInt(da + db) && p >= 10n ** BigInt(da + db - 2);
  if (!ok || !inRange) { fails++; if (fails < 5) __L(0, "FAIL i=" + i + " da=" + da + " db=" + db); }
  feed += da + "x" + db + ":" + String(p).length + "\n";
}
// zero/neg/one identities
var z = 0n, one = 1n, neg = -1n;
var a = bigRnd(1500);
if (a * z !== 0n || z * a !== 0n) fails++;
if (a * one !== a || a * neg !== -a) fails++;
if ((-a) * (-a) !== a * a) fails++;
if (a * a / a !== a) fails++;
feed += "identities\n";
__L(1, "fails=" + fails + " hash=" + fnv(feed));

summary("builtins_ext");
