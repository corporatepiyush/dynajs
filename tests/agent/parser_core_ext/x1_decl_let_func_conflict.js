// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = null;
// X1: DIVERGENCE DOCUMENTATION (expected FAIL vs node — pre-existing, baseline-equal).
// dynajs accepts `let rg; function rg() {}` in one script (indirect eval);
// the spec requires an early SyntaxError (lexical name conflicts with the
// var-scoped function declaration). node rejects; dynajs tree AND pristine
// baseline both accept -> upstream-inherited, NOT introduced by the
// declaration-indexing change. Kept as a permanent differential probe.
function tryEval(s) {
  try { (0, eval)(s); return "ok"; } catch (e) { return e.constructor.name; }
}
__A("x1_decl_let_func_conflict.js:a", function () { assert_eq(tryEval("let rg; function rg() {}"), "SyntaxError", "a"); });
__A("x1_decl_let_func_conflict.js:b", function () { assert_eq(String(typeof globalThis.rg), "undefined", "b"); });
// control: the let-then-var and function-then-let orders
__A("x1_decl_let_func_conflict.js:c", function () { assert_eq(tryEval("let r1; var r1;"), "SyntaxError", "c"); });
__A("x1_decl_let_func_conflict.js:d", function () { assert_eq(tryEval("function r2() {} let r2;"), "SyntaxError", "d"); });

summary("parser_core_ext");
