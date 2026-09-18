// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[1] = ["deep 2:done"];
__EXP[2] = ["deep RangeError"];
__REQ = {"dynajs": {"1": 1}, "node": {"2": 1}};
// WHITELIST-DIVERGE: deep-tail-recursion (dynajs PTC succeeds where node RangeErrors — intentional)
// P10: sloppy `arguments` object live across tail hops — mapped-arguments
// teardown happens per frame; final frame must see its own arguments/length.
function rec(n) {
  if (n === 0) return arguments.length + ":done";
  return rec(n - 1, n);
}
__A("p10_ptc_arguments.js:a", function () { assert_eq(rec(3), "2:done", "a"); });
try { __L(1, "deep", rec(250000)); } catch (e) { __L(2, "deep", e.constructor.name); }
// arguments use BEFORE the tail call each hop
function rec2(n) {
  if (n <= 0) return "end";
  if (arguments.length !== 2 && arguments.length !== 1) return "bad-len";
  return rec2(n - 1, "x");
}
__A("p10_ptc_arguments.js:b", function () { assert_eq(rec2(3), "end", "b"); });
// delete of an arguments element mid-frame, then tail call
function rec3(n) {
  if (n === 0) return "del-ok";
  var a = arguments;
  delete a[0];
  return rec3(n - 1);
}
__A("p10_ptc_arguments.js:c", function () { assert_eq(rec3(3), "del-ok", "c"); });

summary("parser_core_ext");
