__EXP = {};
__EXP[0] = ["PASS 17_slice_retention"];
var roots = [];
for (var r = 0; r < 20; r++) {
  var p = "";
  for (var i = 0; i < 300; i++) p += String.fromCharCode(32 + ((r * 7 + i) % 90));
  var sl = p.slice(10, 290);
  var sl2 = sl.slice(5, 250);
  roots.push([p, sl, sl2]);
}
for (var g = 0; g < 1000; g++) { var junk = new Array(1000).fill("x").join(""); }
for (var r = 0; r < 20; r++) {
  var p = roots[r][0], sl = roots[r][1], sl2 = roots[r][2];
  eq(sl2, p.slice(15, 260), "retention r" + r);
  eq(sl, p.slice(10, 290), "slice1 r" + r);
  eq(sl.length, 280, "len r" + r);
}
function makeChain() {
  var big = parent(20000);
  var a = big.slice(100, 15000);
  var b = a.slice(50, 10000);
  return b;
}
var kept = makeChain();
for (var g2 = 0; g2 < 300; g2++) { var junk2 = parent(5000); }
ok(kept.length === 9950, "kept len");
eq(kept.charCodeAt(0), kept.charCodeAt(0), "kept readable");
__L(0, "PASS 17_slice_retention");

summary("sliced_strings");
