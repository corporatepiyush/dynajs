// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["A1 let;func SyntaxError", "A2 let;async func SyntaxError", "A3 let;gen SyntaxError", "A4 let;async gen SyntaxError", "A5 const;func SyntaxError", "B1 func;let SyntaxError", "B2 let;var SyntaxError", "B3 var;func ok", "B4 func;func ok", "B5 func;var ok", "C1 let;{func} ok", "C2 let;if(0)func ok", "C3 let;if(1){func} ok", "C4 let;sw{case:func} ok", "C5 var;if(1){func} ok", "D1 let;func -> typeof ok", "E1 typeof after A1 fail ok", "F1 twice-nested block ok", "F2 let in inner block, func global ok"];
// x1 matrix: global/eval-scope function-declaration vs lexical name conflicts.
// Every case runs in a FRESH indirect eval (GlobalDeclarationInstantiation-like).
function t(tag, s) {
  var r;
  try { (0, eval)(s); r = "ok"; } catch (e) { r = e.constructor.name; }
  __L(0, tag, r);
}
// missing-order (the defect): lexical first, statement function after
t("A1 let;func", "let x1a; function x1a(){}");
t("A2 let;async func", "let x2a; async function x2a(){}");
t("A3 let;gen", "let x3a; function* x3a(){}");
t("A4 let;async gen", "let x4a; async function* x4a(){}");
t("A5 const;func", "const x5a = 1; function x5a(){}");
// control orders (must stay as-is)
t("B1 func;let", "function x1b(){} let x1b;");
t("B2 let;var", "let x2b; var x2b;");
t("B3 var;func", "var x3b; function x3b(){}");
t("B4 func;func", "function x4b(){} function x4b(){}");
t("B5 func;var", "function x5b(){} var x5b;");
// block/if/switch nested functions after a same-name lexical
t("C1 let;{func}", "let x1c; { function x1c(){} }");
t("C2 let;if(0)func", "let x2c; if (0) function x2c(){}");
t("C3 let;if(1){func}", "let x3c; if (1) { function x3c(){} }");
t("C4 let;sw{case:func}", "let x4c; switch(1){case 1: function x4c(){}}");
t("C5 var;if(1){func}", "var x5c; if (1) { function x5c(){} }");
// global-object effects of the missing-order (after each failed/successful decl)
t("D1 let;func -> typeof", "let x1d; try { function x1d(){} } catch(e) {}");
t("E1 typeof after A1 fail", "let x1e; try { function x1e(){} } catch(e){} ");
t("F1 twice-nested block", "let x1f; { { function x1f(){} } }");
t("F2 let in inner block, func global", "{ let x2f; } function x2f(){}");
// sloppy DIRECT eval inside a function (is_global_var true path)
function de(s) { try { eval(s); return "ok"; } catch (e) { return e.constructor.name; } }
console.log("G1 dir let;func", de("let y1; function y1(){}"));
console.log("G2 dir var;func", de("var y2; function y2(){}"));
console.log("G3 dir func;let", de("function y3(){} let y3;"));
console.log("G4 dir let;var", de("let y4; var y4;"));
console.log("G5 dir let;{func}", de("let y5; { function y5(){} }"));
// leak checks
console.log("H1", typeof globalThis.x1a, typeof globalThis.x3b, typeof globalThis.x1b);

summary("parser_core_ext");
