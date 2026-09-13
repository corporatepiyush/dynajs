// fold-active standalone variant of wb1_shadow_fnexpr.js (gen_waveb.py); transcript checked by run_fold.sh
const M = Object.freeze({ S: "hello" });
const g = function M() { return typeof M; };
function __p(s) { if (typeof print === "function") print(s); else console.log(s); }
try { __p("fnexpr=" + String((function(){ return g() === "function" && M.S; })())); } catch (e) { __p("fnexpr=!threw:" + ((e && e.constructor && e.constructor.name) || "unknown")); }
try { __p("top=" + String((function(){ return M.S; })())); } catch (e) { __p("top=!threw:" + ((e && e.constructor && e.constructor.name) || "unknown")); }
