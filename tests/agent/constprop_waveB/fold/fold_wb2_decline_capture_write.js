// fold-active standalone variant of wb2_decline_capture_write.js (gen_waveb.py); transcript checked by run_fold.sh
function f() { const M = Object.freeze({ A: 7 }); return function () { try { M.A = 1; } catch (e) {} return M.A; }; }
var g = f();
function __p(s) { if (typeof print === "function") print(s); else console.log(s); }
try { __p("g=" + String((function(){ return g(); })())); } catch (e) { __p("g=!threw:" + ((e && e.constructor && e.constructor.name) || "unknown")); }
