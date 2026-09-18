// fold-active standalone variant of wb1_shadow_inner_const.js (gen_waveb.py); transcript checked by run_fold.sh
const M = Object.freeze({ S: "hello" });
function f() { const M = Object.freeze({ S: "in" }); return M.S; }
function __p(s) { if (typeof print === "function") print(s); else console.log(s); }
try { __p("inner_const=" + String((function(){ return f() + M.S; })())); } catch (e) { __p("inner_const=!threw:" + ((e && e.constructor && e.constructor.name) || "unknown")); }
try { __p("top=" + String((function(){ return M.S; })())); } catch (e) { __p("top=!threw:" + ((e && e.constructor && e.constructor.name) || "unknown")); }
