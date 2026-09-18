// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["a first-a two d"];
__EXP[9] = ["f x2,x3 evals=1"];
// S6: duplicate case labels, folded switch(true) conditions, null/undefined
// cases, fallthrough ordering with side effects — fused probes must keep
// first-match-wins and evaluation order.
function f(x) {
  switch (x) {
    case "a" + "": return "first-a";
    case "a": return "second-a";
    case 1 + 1: return "two";
    case 2: return "two-again";
    default: return "d";
  }
}
__L(0, "a", f("a"), f(2), f(3));
switch (true) {
  case 1 + 1 === 2: __A("s06_switch_dupes_fold.js:b", function () { assert_eq("t1", "t1", "b"); }); break;
  case true: __L(2, "b", "t2"); break;
  default: __L(3, "b", "td");
}
switch (null) { case null: __A("s06_switch_dupes_fold.js:c", function () { assert_eq("n1", "n1", "c"); }); break; case undefined: __L(5, "c", "n2"); break; }
switch (undefined) { case null: __L(6, "d", "u1"); break; case undefined: __A("s06_switch_dupes_fold.js:d", function () { assert_eq("u2", "u2", "d"); }); break; }
var o = [];
switch (2) {
  case 1 + 1: o.push("c2");
  case 2 + 1: o.push("c3");
  case 1: o.push("c1b"); break;
  case 2: o.push("c2b");
}
__A("s06_switch_dupes_fold.js:e", function () { assert_eq(o.join(","), "c2,c3,c1b", "e"); });
// side-effecting discriminant evaluated exactly once with fallthrough
var evals = 0;
function disc() { evals++; return 1 + 1; }
o.length = 0;
switch (disc()) {
  case 2: o.push("x2");
  case 3: o.push("x3"); break;
  case 4: o.push("x4");
}
__L(9, "f", o.join(","), "evals=" + evals);

summary("parser_core_ext");
