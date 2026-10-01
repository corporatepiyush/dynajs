__EXP = {};
__EXP[0] = ["0 eq=true sign=zero nfcEq=true", "1 eq=true sign=zero nfcEq=true", "2 eq=true sign=zero nfcEq=true", "3 eq=true sign=zero nfcEq=true", "4 eq=true sign=zero nfcEq=true", "5 eq=true sign=zero nfcEq=true", "6 eq=true sign=zero nfcEq=true", "7 eq=true sign=zero nfcEq=true", "8 eq=true sign=zero nfcEq=true", "9 eq=true sign=zero nfcEq=true", "10 eq=true sign=zero nfcEq=true", "11 eq=true sign=zero nfcEq=true"];
__EXP[2] = [["[SIGN] near 0 nonzero=true sign=pos", "[SIGN] near 0 nonzero=true sign=neg"], "[SIGN] near 1 nonzero=true sign=neg", "[SIGN] near 2 nonzero=true sign=neg", ["[SIGN] near 3 nonzero=true sign=pos", "[SIGN] near 3 nonzero=true sign=neg"], ["[SIGN] near 4 nonzero=true sign=neg", "[SIGN] near 4 nonzero=true sign=pos"]];
__EXP[3] = [["[SIGN] hash=70052ac7", "[SIGN] hash=8ecdaa97"]];
__REQ = {"dynajs": {"0": 12, "2": 5, "3": 1}, "node": {"0": 12, "2": 5, "3": 1}};
function fnv(str){var h=0x811c9dc5,i;for(i=0;i<str.length;i++){h^=str.charCodeAt(i);h=Math.imul(h,0x01000193);}return (h>>>0).toString(16);}
var feed = "";
var pairs = [
  ["e\u0301", "\u00e9"],
  ["a\u0300", "\u00e0"],
  ["\u00e9", "e\u0301"],
  ["a\u0328\u0301", "\u0105\u0301"],
  ["\u0105\u0301", "a\u0328\u0301"],
  ["\u1112\u1161\u11ab", "\ud55c"],
  ["\ud55c", "\u1112\u1161\u11ab"],
  ["\u2126", "\u03a9"],
  ["\u00c5", "A\u030a"],
  ["q\u0307\u0323", "q\u0323\u0307"],
  ["\u1e69", "s\u0323\u0307"],
  ["\ufb01", "\ufb01"]
];
for (var i = 0; i < pairs.length; i++) {
  var a = pairs[i][0], b = pairs[i][1];
  var v = a.localeCompare(b);
  var line = i + " eq=" + (v === 0) + " sign=" + (v < 0 ? "neg" : v > 0 ? "pos" : "zero") + " nfcEq=" + (a.normalize("NFC") === b.normalize("NFC"));
  feed += line + "\n";
  __L(0, line);
  if (a.normalize("NFC") === b.normalize("NFC") && v !== 0) __L(1, "FAIL canon-eq-nonzero " + i);
}
var near = [["\u00e9", "\u00e8"], ["a", "b"], ["\u4e16", "\u754c"], ["a\u0301", "a\u0300"], ["A", "a"]];
for (var i = 0; i < near.length; i++) {
  var v = near[i][0].localeCompare(near[i][1]);
  __L(2, "[SIGN] near " + i + " nonzero=" + (v !== 0) + " sign=" + (v < 0 ? "neg" : v > 0 ? "pos" : "zero"));
  feed += "near " + i + " " + (v < 0 ? "neg" : v > 0 ? "pos" : "zero") + "\n";
}
__L(3, "[SIGN] hash=" + fnv(feed));

summary("builtins_ext");
