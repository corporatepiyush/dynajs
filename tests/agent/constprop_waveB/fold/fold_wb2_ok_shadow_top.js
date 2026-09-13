// fold-active standalone variant of wb2_ok_shadow_top.js (gen_waveb.py); transcript checked by run_fold.sh
const T = Object.freeze({ A: 1 });
function f() { const T = Object.freeze({ A: 100 }); return T.A; }
function __p(s) { if (typeof print === "function") print(s); else console.log(s); }
try { __p("f=" + String((function(){ return f(); })())); } catch (e) { __p("f=!threw:" + ((e && e.constructor && e.constructor.name) || "unknown")); }
try { __p("top=" + String((function(){ return T.A; })())); } catch (e) { __p("top=!threw:" + ((e && e.constructor && e.constructor.name) || "unknown")); }
