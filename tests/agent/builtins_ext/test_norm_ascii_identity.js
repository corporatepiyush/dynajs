// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[1] = ["fails=0 hash=704c4aa3"];
// test_norm_ascii_identity.js — all 4 normalization forms on ASCII-only
// strings must be exact identity (fast path must not allocate/alter).
function fnv(str){var h=0x811c9dc5,i;for(i=0;i<str.length;i++){h^=str.charCodeAt(i);h=Math.imul(h,0x01000193);}return (h>>>0).toString(16);}
var forms = ["NFC", "NFD", "NFKC", "NFKD"];
var fails = 0, feed = "";
var samples = ["", "a", "hello world", "The quick brown fox 0123456789 !@#$%",
               "~".repeat(300), "\x7f\x01\x1f control\t\n"];
for (var i = 0; i < samples.length; i++) {
  var s = samples[i];
  for (var f = 0; f < 4; f++) {
    var out = s.normalize(forms[f]);
    if (out !== s) { fails++; __L(0, "FAIL " + forms[f] + " sample " + i); }
  }
  feed += i + ":" + s.length + "\n";
}
// default arg = NFC
if ("ascii".normalize() !== "ascii") fails++;
__L(1, "fails=" + fails + " hash=" + fnv(feed));

summary("builtins_ext");
