// converted from console.log/print oracle: expectations baked from the
// node oracle; dynajs-vs-node divergences are explicit DIVERGE entries.
__EXP = {};
__EXP[0] = ["C2s let;if(0)func ok", "C2t let;if(1)func ok", "K1 try{}catch(x){func} SyntaxError", "K2 catch(x){let x} SyntaxError", "K3 catch(x){var x} ok", "K4 catch-block-func-ref SyntaxError", "K5 label-func SyntaxError", "L1 while-func SyntaxError", "L2 let;for-func-head ok", "L3 do-func SyntaxError"];
function t(tag, s) {
  var r;
  try { (0, eval)(s); r = "ok"; } catch (e) { r = e.constructor.name; }
  __L(0, tag, r);
}
t("C2s let;if(0)func", "let z2; if (0) function z2(){}");
t("C2t let;if(1)func", "let z3; if (1) function z3(){}");
t("K1 try{}catch(x){func}", "try {} catch (z4) { function z4(){} }");
t("K2 catch(x){let x}", "try {} catch (z5) { let z5; }");
t("K3 catch(x){var x}", "try {} catch (z6) { var z6; }");
t("K4 catch-block-func-ref", "try {} catch (z7) { function z7(){ return 7; } } ");
t("K5 label-func", "let z8; lbl: function z8(){}");
t("L1 while-func", "let z9; while(0) function z9(){}");
t("L2 let;for-func-head", "let z10; for (function z10(){};;) break;");
t("L3 do-func", "let z11; do function z11(){} while(0)");

summary("parser_core_ext");
