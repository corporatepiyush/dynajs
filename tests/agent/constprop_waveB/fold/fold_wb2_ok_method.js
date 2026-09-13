// fold-active standalone variant of wb2_ok_method.js (gen_waveb.py); transcript checked by run_fold.sh
var o = { f() { const M = Object.freeze({ A: 3 }); return M.A; } };
function __p(s) { if (typeof print === "function") print(s); else console.log(s); }
try { __p("f=" + String((function(){ return o.f(); })())); } catch (e) { __p("f=!threw:" + ((e && e.constructor && e.constructor.name) || "unknown")); }
