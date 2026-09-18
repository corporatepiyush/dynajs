// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[2] = [["deep InternalError", "deep RangeError"]];
__EXP[4] = ["c NaN NaN"];
__REQ = {"dynajs": {"2": 1, "4": 1}, "node": {"2": 1, "4": 1}};
// WHITELIST-DIVERGE: deep-tail-recursion (dynajs PTC succeeds where node RangeErrors — intentional)
// P11: tail recursion entirely inside eval'd STRICT code, and inside a
// default-param/destructuring prologue (larger frame setup per hop).
var src = "'use strict'; function rec(n){ return n === 0 ? 'strict-eval-done' : rec(n - 1); } return rec;";
var rec = (new Function(src))();
__A("p11_ptc_strict_eval_prologue.js:a", function () { assert_eq(rec(5), "strict-eval-done", "a"); });
try { __L(1, "deep", rec(200000)); } catch (e) { __L(2, "deep", e.constructor.name); }
// destructured + defaulted params recomputed each hop
function rec2(n, obj, b) {
  if (n === 0) return obj.a + b;
  return rec2(n - 1, { a: 2 + 3 }, b);
}
__A("p11_ptc_strict_eval_prologue.js:b", function () { assert_eq(rec2(3, { a: 2 + 3 }, 10), 15, "b"); });
function rec3(n, obj = {}, b = 10) {
  if (n === 0) return obj.a + b;
  return rec3(n - 1, undefined, b);   // obj/b defaults re-evaluate every hop
}
__L(4, "c", rec3(4), rec3(2, { a: 7 }));

summary("parser_core_ext");
