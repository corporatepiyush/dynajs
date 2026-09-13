// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[1] = ["fails=0"];
// test_str_intern_map_identity.js — interning identity of cached 1-char
// strings: literal vs charAt vs split("") element vs fromCharCode vs string
// index read must all be the SAME Map key / object key (atom donation).
function fnv(str){var h=0x811c9dc5,i;for(i=0;i<str.length;i++){h^=str.charCodeAt(i);h=Math.imul(h,0x01000193);}return (h>>>0).toString(16);}
var fails = 0;
function chk(c, l) { if (!c) { fails++; __L(0, "FAIL " + l); } }
var alphabet = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
var m = new Map();
for (var i = 0; i < alphabet.length; i++) {
  var ch = alphabet.charAt(i);
  m.set(ch, 1);
  m.set(ch.charAt(0), 2);          // cached 1-char from charAt
  m.set(alphabet[i], 3);           // index read
  m.set(String.fromCharCode(alphabet.charCodeAt(i)), 4);
}
chk(m.size === alphabet.length, "map-size " + m.size);
for (var i = 0; i < alphabet.length; i++) chk(m.get(alphabet.charAt(i)) === 4, "map-val " + i);
// split("") products as Map keys
var parts = "hello world".split("");
var m2 = new Map();
m2.set("h", "lit"); m2.set(parts[0], "split"); m2.set("hello".charAt(0), "charat");
chk(m2.size === 1 && m2.get("h") === "charat", "map-single-key");
// object keys likewise
var o = {};
o["k"] = 1; o["kk".charAt(1)] = 2; o[String.fromCharCode(107)] = 3;
chk(Object.keys(o).length === 1 && o["k"] === 3, "obj-key-identity");
// equality across ALL creation paths incl ===
var c = "q";
chk((c === "q") && (c === "qq".charAt(1)) && (c === "q".split("")[0]) && (c === String.fromCharCode(113)), "strict-eq-paths");
// wide chars (2-byte atoms) and the 0x7F/0x80 boundary
var w = "\u00e9";
chk(w === "\u00e9".charAt(0) && w === "\u00e9\u0301".charAt(0), "wide-eq");
chk(w === String.fromCharCode(0xe9), "wide-fromcc");
var b7f = String.fromCharCode(0x7f), b80 = String.fromCharCode(0x80);
chk((b7f < b80) && b7f !== b80, "7f-80-distinct");
var mo = new Map(); mo.set(b80, 1); mo.set(String.fromCharCode(0x80), 2); mo.set("\u0080".split("")[0], 3);
chk(mo.size === 1, "map-80-identity " + mo.size);
// numbers-to-string keys are NOT the same atoms as char caches
var o2 = {};
o2[1] = "num"; o2["1".charAt(0)] = "char";
chk(o2[1] === "char" && o2["1"] === "char", "numeric-key-coerce");
__L(1, "fails=" + fails);

summary("builtins_ext");
