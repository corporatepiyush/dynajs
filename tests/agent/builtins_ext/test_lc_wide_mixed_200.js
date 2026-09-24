// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[2] = ["pref 0 ok=true sign=neg", "pref 1 ok=true sign=pos", "pref 2 ok=true sign=neg", "pref 3 ok=true sign=pos"];
__EXP[3] = [["[SIGN] fails=0 invFails=0 hash=417a693b", "[SIGN] fails=0 invFails=28 hash=74949d49"]];
__REQ = {"dynajs": {"2": 4, "3": 1}, "node": {"2": 4, "3": 1}};
// test_lc_wide_mixed_200.js — 200-char wide equal strings, mixed ASCII/wide
// comparisons; engine-internal codepoint-consistency invariant asserted for
// every pair; raw signs are impl-defined vs ICU ([SIGN] tag).
function mulberry32(seed){var s=seed>>>0;return function(){s=(s+0x6D2B79F5)|0;var t=Math.imul(s^(s>>>15),1|s);t=(t+Math.imul(t^(t>>>7),61|t))^t;return (t^(t>>>14))>>>0;};}
function fnv(str){var h=0x811c9dc5,i;for(i=0;i<str.length;i++){h^=str.charCodeAt(i);h=Math.imul(h,0x01000193);}return (h>>>0).toString(16);}
var r = mulberry32(0x0C4A);
var feed = "";
var fails = 0;
// 200-char wide equal strings
var w = "";
for (var i = 0; i < 200; i++) w += String.fromCharCode(0x3042 + (r() % 0x50));
if (w.localeCompare(w) !== 0) { fails++; __L(0, "WIDE-SELF-NONZERO"); }
var w2 = "";
for (var i = 0; i < 200; i++) w2 += String.fromCharCode(0x3042 + (r() % 0x50));
// equal twins constructed twice (both fast-path eligible)
if (w2.localeCompare(w2) !== 0) { fails++; __L(1, "WIDE2-SELF-NONZERO"); }
feed += "wideSelfOk w!=w2=" + (w !== w2) + "\n";
// mixed ASCII/wide pairs: invariant sign(localeCompare)==sign(codepoint)
var mix = ["a", "\u00e9", "\u4e16", "z", "\u3042", "\uff21", "~", "\u0080", "\u2603"];
var invFails = 0;
for (var i = 0; i < mix.length; i++) {
  for (var j = 0; j < mix.length; j++) {
    var a = mix[i] + "tail", b = mix[j] + "tail";
    var v = a.localeCompare(b);
    var cp = a < b ? -1 : a > b ? 1 : 0;
    if ((v < 0 ? -1 : v > 0 ? 1 : 0) !== cp) invFails++;
  }
}
feed += "invFails=" + invFails + "\n";
// 200-char ASCII self-compare too
var big = "abcdefghij".repeat(20);
feed += "bigSelf=" + (big.localeCompare(big) === 0) + "\n";
// prefix relations (ASCII): stable, must equal node (ICU agrees on prefixes)
var pref = [["abc", "abcd"], ["abcd", "abc"], ["", "a"], ["a", ""]];
for (var i = 0; i < pref.length; i++) {
  var v = pref[i][0].localeCompare(pref[i][1]);
  var expect = pref[i][0] < pref[i][1] ? -1 : pref[i][0] > pref[i][1] ? 1 : 0;
  __L(2, "pref " + i + " ok=" + ((v < 0 ? -1 : v > 0 ? 1 : 0) === expect) + " sign=" + (v < 0 ? "neg" : v > 0 ? "pos" : "zero"));
  feed += "pref " + i + " " + (v < 0 ? "neg" : v > 0 ? "pos" : "zero") + "\n";
}
__L(3, "[SIGN] fails=" + fails + " invFails=" + invFails + " hash=" + fnv(feed));

summary("builtins_ext");
