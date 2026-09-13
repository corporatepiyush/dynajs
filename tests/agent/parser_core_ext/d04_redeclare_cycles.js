// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = null;
// D4: global var + function + let redeclare cycles — each attempt in a fresh
// indirect eval script; Annex-B allowed combos must succeed, let conflicts
// must throw SyntaxError without corrupting the global scope.
function tryEval(s) {
  try { (0, eval)(s); return "ok"; } catch (e) { return e.constructor.name; }
}
__A("d04_redeclare_cycles.js:a", function () { assert_eq(tryEval("var ra; function ra() {}"), "ok", "a"); });
__A("d04_redeclare_cycles.js:b", function () { assert_eq(tryEval("var rb; var rb;"), "ok", "b"); });
__A("d04_redeclare_cycles.js:c", function () { assert_eq(tryEval("let rc; var rc;"), "SyntaxError", "c"); });
__A("d04_redeclare_cycles.js:d", function () { assert_eq(tryEval("var rd; let rd;"), "SyntaxError", "d"); });
__A("d04_redeclare_cycles.js:e", function () { assert_eq(tryEval("let re; let re;"), "SyntaxError", "e"); });
__A("d04_redeclare_cycles.js:f", function () { assert_eq(tryEval("function rf() {} let rf;"), "SyntaxError", "f"); });
__A("d04_redeclare_cycles.js:g", function () { assert_eq(tryEval("function rg2() { return 1; } var rg2;"), "ok", "g"); });
__A("d04_redeclare_cycles.js:h", function () { assert_eq(tryEval("var rh = 1; { let rh; }"), "ok", "h"); });
console.log("i", tryEval("const ri = 1; ri = 2;"));
console.log("j", String(typeof ra), String(typeof rb));
console.log("k", String(typeof rc), String(typeof rd), String(typeof re), String(typeof rf), String(typeof rg2));
// same cycles inside direct eval (function scope, sloppy)
function cycles() {
  var out = [];
  function t(s) { try { eval(s); out.push("ok"); } catch (e) { out.push(e.constructor.name); } }
  t("var sa; function sa() {}");
  t("let sc; var sc;");
  t("function sf() {} let sf;");
  t("var sg = 1; { let sg = 2; out.push(sg); }");
  return out.join(",") + "|" + (typeof sa) + "," + (typeof sg === "undefined" ? "undef" : "def");
}
console.log("l", cycles());

summary("parser_core_ext");
