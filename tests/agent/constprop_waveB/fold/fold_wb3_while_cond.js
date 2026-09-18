// fold-active standalone variant of wb3_while_cond.js (gen_waveb.py); transcript checked by run_fold.sh
const M = Object.freeze({ A: 2, L: 3 });
function f() { let n = 0; while (n < M.L) { n += M.A; } return n; }
function __p(s) { if (typeof print === "function") print(s); else console.log(s); }
try { __p("f=" + String((function(){ return f(); })())); } catch (e) { __p("f=!threw:" + ((e && e.constructor && e.constructor.name) || "unknown")); }
