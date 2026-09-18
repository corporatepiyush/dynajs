// fold-active standalone variant of wb2_decline_not_first.js (gen_waveb.py); transcript checked by run_fold.sh
function f() { var t = 1; const M = Object.freeze({ A: 8 }); return t + M.A; }
function __p(s) { if (typeof print === "function") print(s); else console.log(s); }
try { __p("f=" + String((function(){ return f(); })())); } catch (e) { __p("f=!threw:" + ((e && e.constructor && e.constructor.name) || "unknown")); }
