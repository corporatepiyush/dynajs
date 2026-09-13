// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = [["[SIGN] a/B sign=pos cpSign=pos", "[SIGN] a/B sign=neg cpSign=pos"], "[SIGN] A/b sign=neg cpSign=neg", "[SIGN] z/A sign=pos cpSign=pos", ["[SIGN] a/Z sign=pos cpSign=pos", "[SIGN] a/Z sign=neg cpSign=pos"], ["[SIGN] Z/a sign=neg cpSign=neg", "[SIGN] Z/a sign=pos cpSign=neg"], "[SIGN] b/A sign=pos cpSign=pos", "[SIGN] A/z sign=neg cpSign=neg", ["[SIGN] q/Q sign=pos cpSign=pos", "[SIGN] q/Q sign=neg cpSign=pos"], ["[SIGN] Q/q sign=neg cpSign=neg", "[SIGN] Q/q sign=pos cpSign=neg"], "[SIGN] a/a sign=eq cpSign=eq", ["[SIGN] C/b sign=neg cpSign=neg", "[SIGN] C/b sign=pos cpSign=neg"], "[SIGN] c/B sign=pos cpSign=pos", ["[SIGN] y/Z sign=pos cpSign=pos", "[SIGN] y/Z sign=neg cpSign=pos"], ["[SIGN] Y/e sign=neg cpSign=neg", "[SIGN] Y/e sign=pos cpSign=neg"], "[SIGN] 0/a sign=neg cpSign=neg", "[SIGN] a/0 sign=pos cpSign=pos", ["[SIGN] _/Z sign=pos cpSign=pos", "[SIGN] _/Z sign=neg cpSign=pos"], "[SIGN] z/_ sign=pos cpSign=pos"];
__EXP[1] = [["[SIGN] invariantFails=0", "[SIGN] invariantFails=9"]];
__EXP[2] = [["[SIGN] wordInvariantFails=0", "[SIGN] wordInvariantFails=29"]];
__EXP[3] = [["[SIGN] hash=bff50711", "[SIGN] hash=391917c1"]];
__REQ = {"dynajs": {"0": 18, "1": 1, "2": 1, "3": 1}, "node": {"0": 18, "1": 1, "2": 1, "3": 1}};
// test_lc_ascii_case_signs.js — ASCII case-differing pairs: the SIGN is
// impl-defined (dynajs: codepoint order; node: ICU collation). Rows tagged
// [SIGN] vs node; in-engine invariant (sign(localeCompare) === sign of
// codepoint compare) is asserted and must hold for EVERY pair.
function fnv(str){var h=0x811c9dc5,i;for(i=0;i<str.length;i++){h^=str.charCodeAt(i);h=Math.imul(h,0x01000193);}return (h>>>0).toString(16);}
var feed = "";
var pairs = [["a","B"],["A","b"],["z","A"],["a","Z"],["Z","a"],["b","A"],["A","z"],["q","Q"],["Q","q"],["a","a"],["C","b"],["c","B"],["y","Z"],["Y","e"],["0","a"],["a","0"],["_","Z"],["z","_"]];
var invFail = 0;
for (var i = 0; i < pairs.length; i++) {
  var a = pairs[i][0], b = pairs[i][1];
  var v = a.localeCompare(b);
  var sign = v < 0 ? "neg" : v > 0 ? "pos" : "eq";
  // engine-internal invariant: localeCompare must agree with codepoint compare
  var cpSign = a < b ? "neg" : a > b ? "pos" : "eq";
  if (sign !== cpSign) invFail++;
  var line = "[SIGN] " + a + "/" + b + " sign=" + sign + " cpSign=" + cpSign;
  feed += line + "\n";
  __L(0, line);
}
__L(1, "[SIGN] invariantFails=" + invFail);
feed += "invariantFails=" + invFail + "\n";
// mixed-case words (impl-defined ordering vs ICU; invariant must hold)
var words = ["Apple","apple","BANANA","banana","Cherry","cherry","applePie","ApplePie"];
for (var i = 0; i < words.length; i++) {
  for (var j = 0; j < words.length; j++) {
    var v = words[i].localeCompare(words[j]);
    var cp = words[i] < words[j] ? -1 : words[i] > words[j] ? 1 : 0;
    if ((v < 0 ? -1 : v > 0 ? 1 : 0) !== cp) invFail++;
  }
}
__L(2, "[SIGN] wordInvariantFails=" + invFail);
feed += "wordInvariantFails=" + invFail + "\n";
__L(3, "[SIGN] hash=" + fnv(feed));

summary("builtins_ext");
