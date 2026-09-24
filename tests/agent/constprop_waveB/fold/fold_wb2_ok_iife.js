// fold-active standalone variant of wb2_ok_iife.js (gen_waveb.py); transcript checked by run_fold.sh
var r = (function () { const M = Object.freeze({ A: 5 }); return M.A; })();
function __p(s) { if (typeof print === "function") print(s); else console.log(s); }
try { __p("r=" + String((function(){ return r; })())); } catch (e) { __p("r=!threw:" + ((e && e.constructor && e.constructor.name) || "unknown")); }
