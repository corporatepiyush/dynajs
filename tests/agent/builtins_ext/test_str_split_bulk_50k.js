// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["fails=0 acc=372786081 hash=fb4bac0d len=50000"];
// test_str_split_bulk_50k.js — split("") bulk loop on 50k ASCII chars:
// length, spot chars, join identity, per-char equality with charAt walk.
function mulberry32(seed){var s=seed>>>0;return function(){s=(s+0x6D2B79F5)|0;var t=Math.imul(s^(s>>>15),1|s);t=(t+Math.imul(t^(t>>>7),61|t))^t;return (t^(t>>>14))>>>0;};}
function fnv(str){var h=0x811c9dc5,i;for(i=0;i<str.length;i++){h^=str.charCodeAt(i);h=Math.imul(h,0x01000193);}return (h>>>0).toString(16);}
var r = mulberry32(0x5A11A);
var s = "";
for (var i = 0; i < 50000; i++) s += String.fromCharCode(32 + (r() % 95));
var a = s.split("");
var fails = 0;
if (a.length !== 50000) fails++;
for (var k = 0; k < 50000; k += 498) if (a[k] !== s.charAt(k)) fails++;
var joined = a.join("");
if (joined !== s) fails++;
for (var k = 0; k < 50000; k += 997) if (a[k].charCodeAt(0) !== s.charCodeAt(k)) fails++;
// bulk map through char codes (forces materialized reads of cached chars)
var acc = 0;
for (var k = 0; k < 50000; k++) acc = (acc + a[k].charCodeAt(0) * 31 + k) % 1000000007;
__L(0, "fails=" + fails + " acc=" + acc + " hash=" + fnv(a.join("|")) + " len=" + s.length);

summary("builtins_ext");
