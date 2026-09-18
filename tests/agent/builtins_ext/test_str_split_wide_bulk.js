// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["fails=0 acc=742736005 hash=5e4233af"];
// test_str_split_wide_bulk.js — split("") bulk on 30k two-byte (wide) chars
// across BMP ranges incl. the 0x7FF/0x800 atom-width boundary.
function mulberry32(seed){var s=seed>>>0;return function(){s=(s+0x6D2B79F5)|0;var t=Math.imul(s^(s>>>15),1|s);t=(t+Math.imul(t^(t>>>7),61|t))^t;return (t^(t>>>14))>>>0;};}
function fnv(str){var h=0x811c9dc5,i;for(i=0;i<str.length;i++){h^=str.charCodeAt(i);h=Math.imul(h,0x01000193);}return (h>>>0).toString(16);}
var r = mulberry32(0x51D3);
var s = "";
for (var i = 0; i < 30000; i++) {
  var c = 0x80 + (r() % 0x2F00); // 0x80..0x2F7F: latin1-ext, Greek, CJK, 0x7FF/0x800 boundary
  if (c >= 0xD800 && c <= 0xDFFF) c += 0x800; // skip surrogates
  s += String.fromCharCode(c);
}
var a = s.split("");
var fails = 0;
if (a.length !== 30000) fails++;
if (a.join("") !== s) fails++;
for (var k = 0; k < 30000; k += 331) if (a[k].charCodeAt(0) !== s.charCodeAt(k)) fails++;
var acc = 0;
for (var k = 0; k < 30000; k++) acc = (acc + a[k].charCodeAt(0) * 7 + k) % 1000000007;
__L(0, "fails=" + fails + " acc=" + acc + " hash=" + fnv(a.join("^")));

summary("builtins_ext");
