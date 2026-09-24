// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["ascii lo===true idemp=true"];
__EXP[1] = ["ascii upHash=a468b578 loHash=a9e76f78"];
__EXP[2] = ["b low=5475aeb9 up=2d37f0cd"];
__EXP[3] = ["sigma lo=f71757d up=96298f4"];
__EXP[4] = ["loclo=6d0757b2"];
// test_str_caseconv_boundary.js — toLowerCase/toUpperCase ASCII fast path vs
// wide path across the 0x7F/0x80 boundary and Turkish-adjacent (non-locale)
// defaults; 50k-char bulk case conv must be byte-identical to node.
function mulberry32(seed){var s=seed>>>0;return function(){s=(s+0x6D2B79F5)|0;var t=Math.imul(s^(s>>>15),1|s);t=(t+Math.imul(t^(t>>>7),61|t))^t;return (t^(t>>>14))>>>0;};}
function fnv(str){var h=0x811c9dc5,i;for(i=0;i<str.length;i++){h^=str.charCodeAt(i);h=Math.imul(h,0x01000193);}return (h>>>0).toString(16);}
var r = mulberry32(0xCA5E);
var s = "";
for (var i = 0; i < 50000; i++) s += String.fromCharCode(33 + (r() % 94)); // ASCII printable
var lo = s.toLowerCase(), up = s.toUpperCase();
__L(0, "ascii lo===" + (lo === s.toLowerCase()) + " idemp=" + (lo === lo.toLowerCase()));
__L(1, "ascii upHash=" + fnv(up) + " loHash=" + fnv(lo));
// boundary + wide battery
var b = "\u007e\u007f\u0080\u00ff\u00c0\u00e0\u0100\u017f\u4e00";
__L(2, "b low=" + fnv(b.toLowerCase()) + " up=" + fnv(b.toUpperCase()));
// Greek sigma: final sigma handling in default (non-locale) case conv
__L(3, "sigma lo=" + fnv("\u03a3\u03c2\u039f".toLowerCase()) + " up=" + fnv("\u03a3\u03c2\u039f".toUpperCase()));
// toLocaleLowerCase is impl-defined for some locales; test the no-arg form only
__L(4, "loclo=" + fnv("ABC\u00c9\u4e00".toLocaleLowerCase()));

summary("builtins_ext");
