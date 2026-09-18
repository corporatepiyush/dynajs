// fold-active standalone variant of wb2_ok_ctor.js (gen_waveb.py); transcript checked by run_fold.sh
function C() { const M = Object.freeze({ A: 4 }); this.v = M.A; }
function __p(s) { if (typeof print === "function") print(s); else console.log(s); }
try { __p("f=" + String((function(){ return new C().v; })())); } catch (e) { __p("f=!threw:" + ((e && e.constructor && e.constructor.name) || "unknown")); }
