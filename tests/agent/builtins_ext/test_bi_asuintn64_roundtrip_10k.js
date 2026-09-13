// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[1] = ["fails=0 hash=ad223bec"];
// test_bi_asuintn64_roundtrip_10k.js — BigInt.asUintN(64) / asIntN(64)
// round-trips on 10k-digit values: u = asUintN(64,x) must satisfy
// x === u + (2n**64n)*k for integer k, i.e. (x - u) divisible by 2^64;
// asIntN(64, x) must be in [-2^63, 2^63).
function mulberry32(seed){var s=seed>>>0;return function(){s=(s+0x6D2B79F5)|0;var t=Math.imul(s^(s>>>15),1|s);t=(t+Math.imul(t^(t>>>7),61|t))^t;return (t^(t>>>14))>>>0;};}
function fnv(str){var h=0x811c9dc5,i;for(i=0;i<str.length;i++){h^=str.charCodeAt(i);h=Math.imul(h,0x01000193);}return (h>>>0).toString(16);}
var r = mulberry32(0xA5E1);
function bigFromDigits(seed2, ndig) {
  var rr = mulberry32(seed2), s = String(1 + (rr() % 9));
  for (var i = 1; i < ndig; i++) s += String(rr() % 10);
  return BigInt(s);
}
var MOD = 2n ** 64n;
var fails = 0, feed = "";
var sizes = [10000, 9997, 5000, 1024];
for (var sIdx = 0; sIdx < sizes.length; sIdx++) {
  for (var rep = 0; rep < 3; rep++) {
    var x = bigFromDigits(0x50 + sIdx * 10 + rep, sizes[sIdx]);
    if (rep === 1) x = -x;
    var u = BigInt.asUintN(64, x);
    var si = BigInt.asIntN(64, x);
    var okU = (x - u) % MOD === 0n && u >= 0n && u < MOD;
    var okS = si >= -(2n ** 63n) && si < 2n ** 63n && (x - si) % MOD === 0n;
    // asIntN is asUintN reinterpreted: si === u - 2^64 if u >= 2^63
    var okRel = si === (u < 2n ** 63n ? u : u - MOD);
    if (!okU || !okS || !okRel) { fails++; __L(0, "FAIL size=" + sizes[sIdx] + " rep=" + rep + " u=" + okU + " s=" + okS + " rel=" + okRel); }
    feed += sizes[sIdx] + ":" + rep + ":" + u.toString(16) + ":" + si.toString(16) + "\n";
  }
}
__L(1, "fails=" + fails + " hash=" + fnv(feed));

summary("builtins_ext");
