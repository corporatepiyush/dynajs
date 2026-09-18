// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["++ digits=20000 divOk=true modOk=true", "-+ digits=20001 divOk=true modOk=true", "+- digits=20001 divOk=true modOk=true", "-- digits=20000 divOk=true modOk=true"];
__EXP[1] = ["uneven digits=20000 ok=true"];
__EXP[2] = ["fails=0 hash=5283fea"];
// test_bi_karatsuba_signs_10k.js — Karatsuba with negative operands, all four
// sign combinations at 10k digits: verified by division/mod identities
// (a*b)/a === b, (a*b)%a === 0n, and cross-sign consistency.
function mulberry32(seed){var s=seed>>>0;return function(){s=(s+0x6D2B79F5)|0;var t=Math.imul(s^(s>>>15),1|s);t=(t+Math.imul(t^(t>>>7),61|t))^t;return (t^(t>>>14))>>>0;};}
function fnv(str){var h=0x811c9dc5,i;for(i=0;i<str.length;i++){h^=str.charCodeAt(i);h=Math.imul(h,0x01000193);}return (h>>>0).toString(16);}
var r = mulberry32(0xB1A11);
function bigFromDigits(seed2, ndig) {
  var rr = mulberry32(seed2), s = String(1 + (rr() % 9));
  for (var i = 1; i < ndig; i++) s += String(rr() % 10);
  return BigInt(s);
}
var a = bigFromDigits(0xA11, 10000);
var b = bigFromDigits(0xB22, 10000);
var fails = 0, feed = "";
var combos = [[a, b, "++"], [-a, b, "-+"], [a, -b, "+-"], [-a, -b, "--"]];
for (var c = 0; c < combos.length; c++) {
  var x = combos[c][0], y = combos[c][1], tag = combos[c][2];
  var p = x * y;
  var q = p / x, m = p % x;
  var ok = q === y && m === 0n;
  if (!ok) fails++;
  var line = tag + " digits=" + String(p).length + " divOk=" + (q === y) + " modOk=" + (m === 0n);
  feed += line + "\n";
  __L(0, line);
}
// cross-consistency: (-a)*b === -(a*b) === a*(-b)
var p0 = a * b;
if ((-a) * b !== -p0) fails++;
if (a * (-b) !== -p0) fails++;
if ((-a) * (-b) !== p0) fails++;
// mixed sizes 10k x 1 digit (karatsuba fallback edge)
var small = 7n;
if ((a * small) / a !== small) fails++;
if ((a * -small) / a !== -small) fails++;
feed += "small-ok\n";
// uneven sizes 9999 x 10001 digits
var u = bigFromDigits(0xC33, 9999), v = bigFromDigits(0xD44, 10001);
var uv = u * v;
if (uv / u !== v || uv % u !== 0n) fails++;
__L(1, "uneven digits=" + String(uv).length + " ok=" + (uv / u === v));
feed += "uneven " + String(uv).length + "\n";
__L(2, "fails=" + fails + " hash=" + fnv(feed));

summary("builtins_ext");
