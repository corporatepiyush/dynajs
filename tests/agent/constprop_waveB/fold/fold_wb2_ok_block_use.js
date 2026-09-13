// fold-active standalone variant of wb2_ok_block_use.js (gen_waveb.py); transcript checked by run_fold.sh
function f(c) { const M = Object.freeze({ A: 9 });
    if (c) { return M.A; } return -M.A; }
function __p(s) { if (typeof print === "function") print(s); else console.log(s); }
try { __p("t=" + String((function(){ return f(true); })())); } catch (e) { __p("t=!threw:" + ((e && e.constructor && e.constructor.name) || "unknown")); }
try { __p("u=" + String((function(){ return f(false); })())); } catch (e) { __p("u=!threw:" + ((e && e.constructor && e.constructor.name) || "unknown")); }
