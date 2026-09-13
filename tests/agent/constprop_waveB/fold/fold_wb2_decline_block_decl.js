// fold-active standalone variant of wb2_decline_block_decl.js (gen_waveb.py); transcript checked by run_fold.sh
function f() { { const M = Object.freeze({ A: 5 }); return M.A; } }
function __p(s) { if (typeof print === "function") print(s); else console.log(s); }
try { __p("f=" + String((function(){ return f(); })())); } catch (e) { __p("f=!threw:" + ((e && e.constructor && e.constructor.name) || "unknown")); }
