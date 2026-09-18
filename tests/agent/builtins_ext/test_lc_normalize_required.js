// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["0 eq=true sign=zero nfcEq=true", "1 eq=true sign=zero nfcEq=true", "2 eq=true sign=zero nfcEq=true", "3 eq=true sign=zero nfcEq=true", "4 eq=true sign=zero nfcEq=true", "5 eq=true sign=zero nfcEq=true", "6 eq=true sign=zero nfcEq=true", "7 eq=true sign=zero nfcEq=true", "8 eq=true sign=zero nfcEq=true", "9 eq=true sign=zero nfcEq=true", "10 eq=true sign=zero nfcEq=true", "11 eq=true sign=zero nfcEq=true"];
__EXP[2] = [["[SIGN] near 0 nonzero=true sign=pos", "[SIGN] near 0 nonzero=true sign=neg"], "[SIGN] near 1 nonzero=true sign=neg", "[SIGN] near 2 nonzero=true sign=neg", ["[SIGN] near 3 nonzero=true sign=pos", "[SIGN] near 3 nonzero=true sign=neg"], ["[SIGN] near 4 nonzero=true sign=neg", "[SIGN] near 4 nonzero=true sign=pos"]];
__EXP[3] = [["[SIGN] hash=70052ac7", "[SIGN] hash=8ecdaa97"]];
__REQ = {"dynajs": {"0": 12, "2": 5, "3": 1}, "node": {"0": 12, "2": 5, "3": 1}};
// test_lc_normalize_required.js — canonically-equivalent pairs MUST compare 0
// (the fast path may NOT raw-compare pairs that need normalization):
// NFD decompositions, reordered combining marks, Hangul jamo compositions.
// Non-equivalent near-pairs must be NONZERO. Byte-identical vs node.
function fnv(str){var h=0x811c9dc5,i;for(i=0;i<str.length;i++){h^=str.charCodeAt(i);h=Math.imul(h,0x01000193);}return (h>>>0).toString(16);}
var feed = "";
var pairs = [
  ["e\u0301", "\u00e9"],                       // e + combining acute vs é
  ["a\u0300", "\u00e0"],
  ["\u00e9", "e\u0301"],
  ["a\u0328\u0301", "\u0105\u0301"],           // reordered combining marks
  ["\u0105\u0301", "a\u0328\u0301"],
  ["\u1112\u1161\u11ab", "\ud55c"],            // Hangul jamo vs precomposed
  ["\ud55c", "\u1112\u1161\u11ab"],
  ["\u2126", "\u03a9"],                        // ohm sign vs omega (canonical equiv)
  ["\u00c5", "A\u030a"],                       // angstrom
  ["q\u0307\u0323", "q\u0323\u0307"],          // canonical ordering
  ["\u1e69", "s\u0323\u0307"],                 // double decomposition
  ["\ufb01", "\ufb01"]                         // ffi ligature (NOT decomposed by NFC — equal only to itself)
];
for (var i = 0; i < pairs.length; i++) {
  var a = pairs[i][0], b = pairs[i][1];
  var v = a.localeCompare(b);
  var line = i + " eq=" + (v === 0) + " sign=" + (v < 0 ? "neg" : v > 0 ? "pos" : "zero") + " nfcEq=" + (a.normalize("NFC") === b.normalize("NFC"));
  feed += line + "\n";
  __L(0, line);
  if (a.normalize("NFC") === b.normalize("NFC") && v !== 0) __L(1, "FAIL canon-eq-nonzero " + i);
}
// near-miss pairs must be nonzero
var near = [["\u00e9", "\u00e8"], ["a", "b"], ["\u4e16", "\u754c"], ["a\u0301", "a\u0300"], ["A", "a"]];
for (var i = 0; i < near.length; i++) {
  var v = near[i][0].localeCompare(near[i][1]);
  __L(2, "[SIGN] near " + i + " nonzero=" + (v !== 0) + " sign=" + (v < 0 ? "neg" : v > 0 ? "pos" : "zero"));
  feed += "near " + i + " " + (v < 0 ? "neg" : v > 0 ? "pos" : "zero") + "\n";
}
__L(3, "[SIGN] hash=" + fnv(feed));

summary("builtins_ext");
