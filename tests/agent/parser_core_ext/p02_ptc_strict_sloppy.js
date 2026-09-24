// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[2] = ["deep s-done"];
__EXP[3] = ["deep RangeError"];
__REQ = {"dynajs": {"2": 1}, "node": {"3": 1}};
// WHITELIST-DIVERGE: deep-tail-recursion (dynajs PTC succeeds where node RangeErrors — intentional)
// P2: strict <-> sloppy alternation in a tail-call chain — frame flags change
// every hop; the trampoline must keep rebinding cleanly.
function strictFn(n) {
  "use strict";
  if (n === 0) return "s-done";
  return sloppyFn(n - 1);
}
function sloppyFn(n) {
  if (n === 0) return "p-done";
  return strictFn(n - 1);
}
__A("p02_ptc_strict_sloppy.js:a", function () { assert_eq(strictFn(7), "p-done", "a"); });
__A("p02_ptc_strict_sloppy.js:b", function () { assert_eq(sloppyFn(8), "p-done", "b"); });
try { __L(2, "deep", strictFn(250000)); } catch (e) { __L(3, "deep", e.constructor.name); }
// sloppy caller using `this` + strict callee (this binding differs)
function sloppyThis(n) { return n === 0 ? (this === undefined ? "undef" : "obj") : strictNoThis(n - 1); }
function strictNoThis(n) {
  "use strict";
  return n === 0 ? "strict" : sloppyThis.call(undefined, n - 1);
}
__A("p02_ptc_strict_sloppy.js:c", function () { assert_eq(sloppyThis.call(undefined, 3), "strict", "c"); });
try { __L(5, "deep2", sloppyThis.call(undefined, 150000)); } catch (e) { __A("p02_ptc_strict_sloppy.js:deep2", function () { assert_eq(ov(e), "overflow", "deep2"); }); }
function ov(e) {
  var n = e && e.name;
  return (n === "RangeError" || n === "InternalError") ? "overflow" : "other:" + n;
}

summary("parser_core_ext");
