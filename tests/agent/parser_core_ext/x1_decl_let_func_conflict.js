__EXP = null;
function tryEval(s) {
  try { (0, eval)(s); return "ok"; } catch (e) { return e.constructor.name; }
}
__A("x1_decl_let_func_conflict.js:a", function () { assert_eq(tryEval("let rg; function rg() {}"), "SyntaxError", "a"); });
__A("x1_decl_let_func_conflict.js:b", function () { assert_eq(String(typeof globalThis.rg), "undefined", "b"); });
__A("x1_decl_let_func_conflict.js:c", function () { assert_eq(tryEval("let r1; var r1;"), "SyntaxError", "c"); });
__A("x1_decl_let_func_conflict.js:d", function () { assert_eq(tryEval("function r2() {} let r2;"), "SyntaxError", "d"); });

summary("parser_core_ext");
