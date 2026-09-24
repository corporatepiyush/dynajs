// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["e1 5"];
__EXP[1] = ["e2 4"];
__EXP[3] = ["e4 768"];
__EXP[4] = ["e5 42"];
// F1: int32/string const folding must survive and agree inside eval-compiled code
// (direct, indirect, nested, strict, with TDZ interactions).
__L(0, "e1", eval("1+2*3-4/2"));
__L(1, "e2", eval("eval('10-(2*3)')"));
var ind = eval;
__A("f01_eval_fold.js:e3", function () { assert_eq(ind("'ab'+'cd' === 'abcd'"), true, "e3"); });
__L(3, "e4", (0, eval)("(function(){ 'use strict'; return (4096>>4) | (3<<8); })()"));
__L(4, "e5", eval("var fv = 6*7; fv"));
console.log("e6", eval("let lt = 100+23; lt"));
console.log("e7", eval("(() => 6*7)()"));
console.log("e8", eval("'x'+'y' === 'xy' ? 'yes' : 'no'"));
// eval body whose fold must not leak the eval's constants into the caller's cpool
for (var i = 0; i < 3; i++) console.log("e9", eval("(2+3)*" + i));
// TDZ inside eval'd code with folded initializer order
console.log("e10", (function () {
  try { return eval("tz + (1+1)"); } catch (e) { return "T:" + e.constructor.name; }
})());
console.log("e11", eval("let tz = 5*5; tz"));
// a LATER eval cannot see the previous eval's let (fresh declarative env per eval)
console.log("e12", (function () {
  try { return eval("tz"); } catch (e) { return "T:" + e.constructor.name; }
})());
console.log("e13", typeof tz === "undefined");

summary("parser_core_ext");
