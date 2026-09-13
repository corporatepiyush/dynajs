// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["j1len=41807 j1hash=d6e8b196"];
__EXP[1] = ["j2len=1380 j2hash=21ecafec"];
__EXP[2] = ["j3=true j3hash=a5f8c90f"];
__EXP[3] = ["roundtrip=true"];
// test_str_json_stringify_10k_cached.js — JSON.stringify over 10k cached
// single-char strings (split("") product) incl. control chars, quotes,
// backslash, DEL, latin1-ext: escaping must be byte-identical to node.
function mulberry32(seed){var s=seed>>>0;return function(){s=(s+0x6D2B79F5)|0;var t=Math.imul(s^(s>>>15),1|s);t=(t+Math.imul(t^(t>>>7),61|t))^t;return (t^(t>>>14))>>>0;};}
function fnv(str){var h=0x811c9dc5,i;for(i=0;i<str.length;i++){h^=str.charCodeAt(i);h=Math.imul(h,0x01000193);}return (h>>>0).toString(16);}
var r = mulberry32(0x35A4F);
var s = "";
for (var i = 0; i < 10000; i++) {
  var c = 1 + (r() % 0x2FF); // control chars through latin1-ext
  if (c >= 0xD800 && c <= 0xDFFF) c = 0x41;
  s += String.fromCharCode(c);
}
var arr = s.split("");
var j1 = JSON.stringify(arr);
// object with cached-char keys and values
var o = {};
for (var i = 0; i < 200; i++) { var k = s.charAt(i); o[k] = s.charAt(i + 200); }
var j2 = JSON.stringify(o);
// nested
var j3 = JSON.stringify({a: arr.slice(0, 50), b: o, c: s.charAt(0), d: [arr[1], [arr[2]]]});
__L(0, "j1len=" + j1.length + " j1hash=" + fnv(j1));
__L(1, "j2len=" + j2.length + " j2hash=" + fnv(j2));
__L(2, "j3=" + (j3.length > 1000) + " j3hash=" + fnv(j3));
// round-trip parse must restore the exact array of cached chars
var p1 = JSON.parse(j1);
var same = p1.length === arr.length;
for (var i = 0; i < arr.length; i += 97) if (p1[i] !== arr[i]) same = false;
__L(3, "roundtrip=" + same);

summary("builtins_ext");
