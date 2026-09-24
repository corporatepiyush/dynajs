// fold-active standalone variant of wb2_decline_fn_str_kill.js (gen_waveb.py); transcript checked by run_fold.sh
function f() { const M = Object.freeze({ S: "hello" }); try { M.S = 2; } catch (e) {} return M.S; }
function __p(s) { if (typeof print === "function") print(s); else console.log(s); }
try { __p("f=" + String((function(){ return f(); })())); } catch (e) { __p("f=!threw:" + ((e && e.constructor && e.constructor.name) || "unknown")); }
