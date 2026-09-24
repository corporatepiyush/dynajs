// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["m 16 31"];
__EXP[2] = ["f3 12 28"];
__EXP[3] = ["f4 12 14 NaN"];
__EXP[4] = ["f5 5 42"];
__EXP[7] = ["f8 30!1 caught2!1"];
// F5: folded values crossing label boundaries — the abstract fold state must
// reset at every merge point; expressions merged through jumps keep correct values.
function m(c) {
  var x = (c ? 2 + 3 : 4 + 5) + (c ? 10 + 1 : 20 + 2);
  return x;
}
__L(0, "m", m(true), m(false));
function f2() {
  var s = 0;
  for (var i = 0; i < 4; i++) {
    if (i === 1) continue;
    s += i + (100 + 11);
  }
  return s;
}
__A("f05_label_fold.js:f2", function () { assert_eq(f2(), 338, "f2"); });
function f3(c) { var a; if (c) { a = 1 + 2; } else { a = 3 + 4; } return a * (2 + 2); }
__L(2, "f3", f3(true), f3(false));
function f4(n) {
  var v;
  switch (n) { case 1: v = 5 + 5; break; case 2: v = 6 + 6; break; }
  return v + (1 + 1);
}
__L(3, "f4", f4(1), f4(2), f4(3));
// short-circuit merging with folds on both arms
function f5(c) { return (c && (2 + 3)) || (6 * 7); }
__L(4, "f5", f5(true), f5(false));
// do-while back edge in the middle of a folded accumulation
function f6() {
  var s = 0, i = 0;
  do { s += (i * (3 + 3)); i++; } while (i < 5);
  return s;
}
__A("f05_label_fold.js:f6", function () { assert_eq(f6(), 60, "f6"); });
// labeled block + break between folds
function f7() {
  var out = [];
  blk: {
    out.push(1 + 1);
    if (true) break blk;
    out.push(9 + 9);
  }
  out.push(3 + 3);
  return out.join(",");
}
__A("f05_label_fold.js:f7", function () { assert_eq(f7(), "2,6", "f7"); });
// try/finally between folds (exception edge is a merge point too)
function f8(x) {
  var r;
  try {
    r = 10 + 10;
    if (x) throw "t";
    r = r + (5 + 5);
  } catch (ex) {
    r = "caught" + (1 + 1);
  } finally {
    r = r + "!" + (1 * 1);
  }
  return r;
}
__L(7, "f8", f8(false), f8(true));

summary("parser_core_ext");
