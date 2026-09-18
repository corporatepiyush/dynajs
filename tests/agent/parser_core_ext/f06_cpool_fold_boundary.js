// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["cp 249 35991124 true", "cp 250 36302125 true", "cp 254 37561139 true", "cp 255 37879655 true", "cp 256 38199680 true", "cp 257 38521216 true", "cp 258 38844265 true", "cp 511 174652135 true", "cp 512 175424256 true", "cp 520 181674220 true"];
// F6: deep fold chains at cpool boundaries — functions whose constant pools
// contain N distinct folded int32 constants for N around 255/256/257/511/512.
// Each (1000+i)*i is folded at resolve time into a distinct constant.
function mk(n) {
  var parts = ["(function(){ var s = 0;"];
  for (var i = 0; i < n; i++) parts.push("s += " + (1000 + i) + "*" + i + ";");
  parts.push("return s;})");
  return eval(parts.join(""));
}
var sizes = [249, 250, 254, 255, 256, 257, 258, 511, 512, 520];
for (var k = 0; k < sizes.length; k++) {
  var fn = mk(sizes[k]);
  var v = fn();
  var chk = fn() === v;
  __L(0, "cp", sizes[k], v, chk);
}
// boundary crossed while a switch on folded strings is ALSO present in the
// same function (fused probe consumes cpool indices beyond 255)
function mk2(n) {
  var parts = ["(function(v){ var s = 0;"];
  for (var i = 0; i < n; i++) parts.push("s += " + (7 + i) + "*" + i + ";");
  parts.push("switch (v) {");
  for (var i = 0; i < 40; i++) parts.push("case \"lab" + i + "\": return s + " + i + ";");
  parts.push("default: return s - 1; }})(\"lab23\")");
  return eval(parts.join(""));
}
__A("f06_cpool_fold_boundary.js:mix255", function () { assert_eq(mk2(255), 5721373, "mix255"); });
__A("f06_cpool_fold_boundary.js:mix257", function () { assert_eq(mk2(257), 5855511, "mix257"); });
__A("f06_cpool_fold_boundary.js:mix600", function () { assert_eq(mk2(600), 73078023, "mix600"); });

summary("parser_core_ext");
