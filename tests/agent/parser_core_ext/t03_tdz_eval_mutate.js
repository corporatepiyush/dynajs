// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = null;
// T3: elided loop-var check + direct eval reading and MUTATING the per-iteration copy.
var fns = [];
for (let i = 0; i < 4; i++) {
  fns.push(function () { return eval("i"); });
  eval("i = i + 10");
}
__A("t03_tdz_eval_mutate.js:a", function () { assert_eq(fns.map(f => f()).join(","), "10", "a"); });
// eval introducing a NEW var must not clobber the let copy in the loop body
var log = [];
for (let j = 0; j < 3; j++) {
  eval("var jshadow = j * 100;");
  log.push(j + ":" + jshadow);
}
console.log("b", log.join(","));
// eval'd function declaring its own let of the same name (no leak either way)
var fns2 = [];
for (let k = 0; k < 3; k++) {
  eval("(function(){ let k = 'inner'; })()");
  fns2.push(() => eval("k"));
}
console.log("c", fns2.map(f => f()).join(","));
// indirect eval cannot see the loop scope at all
var ind = eval;
var out = [];
for (let m = 0; m < 2; m++) {
  try { out.push(String(ind("m"))); } catch (e) { out.push("E:" + e.constructor.name); }
}
__A("t03_tdz_eval_mutate.js:d", function () { assert_eq(out.join(","), "E:ReferenceError,E:ReferenceError", "d"); });

summary("parser_core_ext");
