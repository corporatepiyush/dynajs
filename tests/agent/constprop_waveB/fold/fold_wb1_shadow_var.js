// fold-active standalone variant of wb1_shadow_var.js (gen_waveb.py); transcript checked by run_fold.sh
const M = Object.freeze({ S: "hello" });
function h() { var M = { S: "var-shadow" }; return M.S; }
function __p(s) { if (typeof print === "function") print(s); else console.log(s); }
try { __p("var=" + String((function(){ return h(); })())); } catch (e) { __p("var=!threw:" + ((e && e.constructor && e.constructor.name) || "unknown")); }
try { __p("top=" + String((function(){ return M.S; })())); } catch (e) { __p("top=!threw:" + ((e && e.constructor && e.constructor.name) || "unknown")); }
