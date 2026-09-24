// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[1] = ["ROW i=0 r7 len=5917 head=22430012620225046432", "ROW i=0 r13 len=4489 head=3c6b45328451c5936372", "ROW i=3 r7 len=1 head=0", "ROW i=3 r13 len=1 head=0", "ROW i=6 r7 len=5916 head=15210452534443022202", "ROW i=6 r13 len=4488 head=55a782c4015c4a677b43"];
__EXP[2] = ["fails=0 hash=bb4cb22b"];
// test_bi_tostring_radix_odd.js — toString(7)/(13) (odd radices) on 5k-digit
// values vs node, byte-identical; round-trip via Horner accumulation.
function mulberry32(seed){var s=seed>>>0;return function(){s=(s+0x6D2B79F5)|0;var t=Math.imul(s^(s>>>15),1|s);t=(t+Math.imul(t^(t>>>7),61|t))^t;return (t^(t>>>14))>>>0;};}
function fnv(str){var h=0x811c9dc5,i;for(i=0;i<str.length;i++){h^=str.charCodeAt(i);h=Math.imul(h,0x01000193);}return (h>>>0).toString(16);}
var r = mulberry32(0x0DD);
function bigFromDigits(seed2, ndig) {
  var rr = mulberry32(seed2), s = String(1 + (rr() % 9));
  for (var i = 1; i < ndig; i++) s += String(rr() % 10);
  return BigInt(s);
}
var fails = 0, feed = "";
var vals = [bigFromDigits(0xE1, 5000), -bigFromDigits(0xE2, 5000),
            bigFromDigits(0xE3, 4999), 0n, 1n, -1n,
            10n ** 4999n, 2n ** 16600n];
for (var i = 0; i < vals.length; i++) {
  for (var rdIdx = 0; rdIdx < 2; rdIdx++) {
    var rd = rdIdx === 0 ? 7 : 13;
    var s = vals[i].toString(rd);
    feed += i + "/" + rd + ":" + s.length + ":" + fnv(s) + "\n";
    // Horner round-trip
    var back = 0n, B = BigInt(rd);
    var neg = s[0] === "-";
    var t = neg ? s.slice(1) : s;
    for (var k = 0; k < t.length; k++) back = back * B + BigInt(parseInt(t[k], rd));
    if (neg) back = -back;
    if (back !== vals[i]) { fails++; __L(0, "RT-FAIL i=" + i + " r" + rd); }
    if (i % 3 === 0) __L(1, "ROW i=" + i + " r" + rd + " len=" + s.length + " head=" + s.slice(0, 20));
  }
}
__L(2, "fails=" + fails + " hash=" + fnv(feed));

summary("builtins_ext");
