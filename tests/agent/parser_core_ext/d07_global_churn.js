// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[1] = ["b true,true,true,true,true,true,true,true true false"];
__EXP[2] = ["c 1 undefined"];
__EXP[3] = ["d 4 true undefined"];
// D7: declaration lookup after delete/redeclare storms in GLOBAL scope via
// repeated indirect eval — lookup must stay correct (no stale hash entries).
var out = [];
for (var round = 0; round < 5; round++) {
  (0, eval)("var storm" + round + " = " + round + "; function stormFn" + round + "() { return 'f" + round + "'; }");
  out.push(String(globalThis["storm" + round]), globalThis["stormFn" + round]());
  (0, eval)("delete globalThis.storm" + round + "; delete globalThis.stormFn" + round + ";");
  out.push(String(globalThis["storm" + round] === undefined), String(globalThis["stormFn" + round] === undefined));
}
__A("d07_global_churn.js:a", function () { assert_eq(out.join(","), "0,f0,true,true,1,f1,true,true,2,f2,true,true,3,f3,true,true,4,f4,true,true", "a"); });
// 2000 throwaway declarations, each immediately deleted (churn)
var src = [];
for (var i = 0; i < 2000; i++) src.push("var churn" + i + "=" + i + ";");
(0, eval)(src.join(""));
var dels = [];
for (var i = 0; i < 2000; i += 250) dels.push(String(delete globalThis["churn" + i]));
__L(1, "b", dels.join(","), String(globalThis.churn0 === undefined), String(globalThis.churn1999 === undefined));
__L(2, "c", String(globalThis.churn1), String(globalThis.churn1500));
// function re-declared 5 times via eval, latest wins; delete clears
for (var k = 0; k < 5; k++) {
  (0, eval)("function redecl() { return " + k + "; }");
}
__L(3, "d", redecl(), delete globalThis.redecl, typeof globalThis.redecl);

summary("parser_core_ext");
