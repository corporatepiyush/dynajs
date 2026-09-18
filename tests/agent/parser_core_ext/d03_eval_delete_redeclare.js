// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[9] = ["i false number"];
__EXP[10] = ["j true undefined"];
// D3: delete / redeclare sequences through indirect eval — global var and
// function declarations from indirect eval are deletable; re-declaration
// cycles after delete; direct eval adds function-scope vars incrementally.
(0, eval)("var dvar = 1; function dfn() { return 'fn'; }");
console.log("a", typeof globalThis.dvar, typeof globalThis.dfn);
console.log("b", delete globalThis.dvar, delete globalThis.dfn);
console.log("c", typeof globalThis.dvar, typeof globalThis.dfn);
(0, eval)("function dfn() { return 'fn2'; } var dvar = 2;");
console.log("d", globalThis.dfn === undefined ? "gone" : String(globalThis.dfn()));
console.log("e", String(globalThis.dvar));
// incremental direct-eval additions (hash rebuild path in the enclosing function)
function inc() {
  eval("var ea = 1;");
  eval("var eb = 2; var ec = 3;");
  eval("function ed() { return 4; }");
  eval("var ea = 10;");
  return [ea, eb, ec, ed()].join(",");
}
console.log("f", inc());
// delete a binding created by direct sloppy eval var (goes to enclosing function scope? no — stays; verify)
function del2() {
  eval("var zz = 5;");
  try { console.log("g", zz, delete zz); } catch (e) { __L(7, "g", e.constructor.name); }
  return typeof zz;
}
__A("d03_eval_delete_redeclare.js:h", function () { assert_eq(del2(), "undefined", "h"); });
// non-configurable global property (defineProperty, engine-agnostic) is not deletable
Object.defineProperty(globalThis, "ncvar", { value: 1, writable: true, enumerable: true, configurable: false });
__L(9, "i", delete globalThis.ncvar, typeof globalThis.ncvar);
// eval-created var IS deletable (both engines agree)
__L(10, "j", (0, eval)("var hv = 1; delete globalThis.hv;"), typeof globalThis.hv);

summary("parser_core_ext");
