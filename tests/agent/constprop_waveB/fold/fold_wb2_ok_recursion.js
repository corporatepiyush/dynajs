// fold-active standalone variant of wb2_ok_recursion.js (gen_waveb.py); transcript checked by run_fold.sh
function r(n) { const K = Object.freeze({ V: 3 });
    if (n <= 0) return 0; return K.V + r(n - 1); }
function __p(s) { if (typeof print === "function") print(s); else console.log(s); }
try { __p("r4=" + String((function(){ return r(4); })())); } catch (e) { __p("r4=!threw:" + ((e && e.constructor && e.constructor.name) || "unknown")); }
try { __p("r0=" + String((function(){ return r(0); })())); } catch (e) { __p("r0=!threw:" + ((e && e.constructor && e.constructor.name) || "unknown")); }
