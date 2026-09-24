// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[8] = ["eval1 7"];
__EXP[9] = [["~eval2", "~eval2"]];
__EXP[11] = ["eval4 4"];
__EXP[12] = ["eval5 42"];
__EXP[16] = ["done"];
// A: const folding × scope interplay (TDZ shadow, with, eval, labeled breaks)
let x = "outer";
{
  {
    __A("a01_fold_scope.js:fold-tdz1", function () { assert_eq(1 + 2 * 3, 7, "fold-tdz1"); });
    let x = 10;
    __A("a01_fold_scope.js:fold-tdz2", function () { assert_eq((() => x)(), 10, "fold-tdz2"); });
  }
  __A("a01_fold_scope.js:fold-tdz3", function () { assert_eq(x, "outer", "fold-tdz3"); });
}
{
  __A("a01_fold_scope.js:fold-tdz4", function () { assert_eq(40 + 2, 42, "fold-tdz4"); });
  let y = 1;
}
try { __L(4, "fold-tdz5-bad", foldvar); } catch (e) { __A("a01_fold_scope.js:fold-tdz5", function () { assert_eq(e.constructor.name, "ReferenceError", "fold-tdz5"); }); }
let foldvar = 5;

var ws = { a: 1 };
with (ws) {
  __A("a01_fold_scope.js:with1", function () { assert_eq(2 + 3 * 4, 14, "with1"); });
  __A("a01_fold_scope.js:with2", function () { assert_eq(a + (1 << 4), 17, "with2"); });
}
__L(8, "eval1", eval("1+2*3"));
__L(9, "eval2", eval("'12'+'34'"));
var indirect = eval;
__A("a01_fold_scope.js:eval3", function () { assert_eq(indirect("4*5+1"), 21, "eval3"); });
__L(11, "eval4", (0, eval)("10-(2*3)"));
__L(12, "eval5", eval("var q = 6*7; q"));

outer: {
  console.log("lab1", 8 / 2 + 1);
  break outer;
}
inner: for (let i = 0; i < 3; i++) {
  console.log("lab2", i * 2 + 1);
  if (i === 1) { console.log("lab3", 100 + 23); break inner; }
}
__L(16, "done");

summary("bbreview");
