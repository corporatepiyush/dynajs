// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["hash=1acc091a"];
__EXP[1] = ["spot0=1/3/-3"];
__EXP[2] = ["spot1=1e+21/0.000001000"];
__EXP[3] = ["spot2=0.33333333333333331/2.55e+2"];
// test_dtoa_fixed_precision_matrix.js — toFixed/toPrecision/toExponential on
// boundary values vs node (these were not part of the Grisu change; must be
// byte-identical — the fast path must not leak into fixed/precision formats).
function fnv(str){var h=0x811c9dc5,i;for(i=0;i<str.length;i++){h^=str.charCodeAt(i);h=Math.imul(h,0x01000193);}return (h>>>0).toString(16);}
var feed = "";
var vals = [0, -0, 0.5, -0.5, 1/3, 2/3, 1e-7, 123.456, 1e21, 1e-21,
            1.7976931348623157e308, 5e-324, NaN, Infinity, -Infinity,
            2.5, -2.5, 0.000001, 1000000000000000000000, 123456789012345678901234567890];
var out = "";
function row(v, fns) {
  var line = "";
  for (var k = 0; k < fns.length; k++) {
    try { line += fns[k].name + "=" + fns[k].fn(v) + ";"; }
    catch (e) { line += fns[k].name + "=THROW:" + e.constructor.name + ";"; }
  }
  return line;
}
for (var i = 0; i < vals.length; i++) {
  var v = vals[i];
  var line = row(v, [
    { name: "f0", fn: function (x) { return x.toFixed(0); } },
    { name: "f2", fn: function (x) { return x.toFixed(2); } },
    { name: "f7", fn: function (x) { return x.toFixed(7); } },
    { name: "f20", fn: function (x) { return x.toFixed(20); } },
    { name: "p1", fn: function (x) { return x.toPrecision(1); } },
    { name: "p10", fn: function (x) { return x.toPrecision(10); } },
    { name: "p21", fn: function (x) { return x.toPrecision(21); } },
    { name: "e0", fn: function (x) { return x.toExponential(0); } },
    { name: "e5", fn: function (x) { return x.toExponential(5); } },
    { name: "e20", fn: function (x) { return x.toExponential(20); } }
  ]);
  feed += i + ":" + line + "\n";
}
__L(0, "hash=" + fnv(feed));
// spot rows for human review
__L(1, "spot0=" + (0.5).toFixed(0) + "/" + (2.5).toFixed(0) + "/" + (-2.5).toFixed(0));
__L(2, "spot1=" + (1e21).toFixed(2) + "/" + (0.000001).toFixed(9));
__L(3, "spot2=" + (1/3).toPrecision(17) + "/" + (255).toExponential(2));

summary("builtins_ext");
