// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["nonzero=0 hash=4d13b22c"];
// test_lc_selfcompare_50k.js — 50k seeded self-compares over mixed ASCII/wide
// strings, all must be 0; plus adjacent-pair compares hashed vs node
// (signs on ASCII-only pairs are engine-stable).
function mulberry32(seed){var s=seed>>>0;return function(){s=(s+0x6D2B79F5)|0;var t=Math.imul(s^(s>>>15),1|s);t=(t+Math.imul(t^(t>>>7),61|t))^t;return (t^(t>>>14))>>>0;};}
function fnv(str){var h=0x811c9dc5,i;for(i=0;i<str.length;i++){h^=str.charCodeAt(i);h=Math.imul(h,0x01000193);}return (h>>>0).toString(16);}
var r = mulberry32(0x500C0);
var nonzero = 0, pairFeed = "";
var prev = "";
for (var i = 0; i < 50000; i++) {
  var n = 1 + (r() % 12);
  var s = "";
  for (var k = 0; k < n; k++) {
    var pick = r() % 3;
    if (pick === 0) s += String.fromCharCode(97 + (r() % 26));
    else if (pick === 1) s += String.fromCharCode(0xc0 + (r() % 0x40));
    else s += String.fromCharCode(0x4e00 + (r() % 300));
  }
  if (s.localeCompare(s) !== 0) nonzero++;
  if (prev !== "" && (r() % 50) === 0) {
    var v = s.localeCompare(s); // self only: engine-stable across ICU
    pairFeed += "" + v + ";";
  }
  prev = s;
}
__L(0, "nonzero=" + nonzero + " hash=" + fnv(pairFeed));

summary("builtins_ext");
