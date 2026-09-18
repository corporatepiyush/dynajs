// fold-active standalone variant of wb2_decline_generator.js (gen_waveb.py); transcript checked by run_fold.sh
function* g() { const M = Object.freeze({ A: 6 }); yield M.A; }
var it = g();
function __p(s) { if (typeof print === "function") print(s); else console.log(s); }
try { __p("v=" + String((function(){ return it.next().value; })())); } catch (e) { __p("v=!threw:" + ((e && e.constructor && e.constructor.name) || "unknown")); }
