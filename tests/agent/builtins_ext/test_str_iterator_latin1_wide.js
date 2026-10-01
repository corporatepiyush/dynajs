__EXP = {};
__EXP[0] = ["0 it=[97,115,99,105,105] len=5", "1 it=[126,127,128,255,256,257] len=6", "2 it=[97,233,19990,30028] len=4", "3 it=[CP1f600] len=2", "4 it=[97,CP1f600,98] len=4", "5 it=[55296] len=1", "6 it=[56320] len=1", "7 it=[55296,55296] len=2", "8 it=[] len=0"];
__EXP[1] = ["astral splitParts=2 iterSteps=1"];
__EXP[2] = ["proto=[255,256,done:true]"];
__EXP[3] = ["hash=3d7fc3d0"];
function fnv(str){var h=0x811c9dc5,i;for(i=0;i<str.length;i++){h^=str.charCodeAt(i);h=Math.imul(h,0x01000193);}return (h>>>0).toString(16);}
var feed = "";
function iterCodes(s) {
  var out = [];
  for (var c of s) out.push(c.length === 1 ? c.charCodeAt(0) : "CP" + c.codePointAt(0).toString(16));
  return out.join(",");
}
var samples = [
  "ascii",
  "\u007e\u007f\u0080\u00ff\u0100\u0101",
  "a\u00e9\u4e16\u754c",
  "\uD83D\uDE00",
  "a\uD83D\uDE00b",
  "\uD800",
  "\uDC00",
  "\uD800\uD800",
  ""
];
for (var i = 0; i < samples.length; i++) {
  var line = i + " it=[" + iterCodes(samples[i]) + "] len=" + samples[i].length;
  feed += line + "\n";
  __L(0, line);
}
var astral = "\uD83D\uDE00";
var viaSplit = astral.split("").length;
var viaIter = 0;
for (var c of astral) viaIter++;
__L(1, "astral splitParts=" + viaSplit + " iterSteps=" + viaIter);
feed += "astral " + viaSplit + "/" + viaIter + "\n";
var it = "\u00ff\u0100"[Symbol.iterator]();
var proto = [];
var step;
while (!(step = it.next()).done) proto.push(step.value.charCodeAt(0));
proto.push("done:" + it.next().done);
__L(2, "proto=[" + proto.join(",") + "]");
feed += "proto\n";
__L(3, "hash=" + fnv(feed));

summary("builtins_ext");
