// fold-active standalone variant of wb2_ok_multi.js (gen_waveb.py); transcript checked by run_fold.sh
function f() { const A = Object.freeze({ X: 1 });
    const B = Object.freeze({ Y: 2 });
    { const C = Object.freeze({ Z: 3 }); return A.X + B.Y + C.Z; } }
function __p(s) { if (typeof print === "function") print(s); else console.log(s); }
try { __p("f=" + String((function(){ return f(); })())); } catch (e) { __p("f=!threw:" + ((e && e.constructor && e.constructor.name) || "unknown")); }
