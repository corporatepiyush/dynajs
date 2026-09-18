// fold-active standalone variant of wb1_null_arg.js (gen_waveb.py); transcript checked by run_fold.sh
const M = Object.freeze({ N: null });
function __p(s) { if (typeof print === "function") print(s); else console.log(s); }
try { __p("arg0=" + String((function(){ return [1, M.N][1]; })())); } catch (e) { __p("arg0=!threw:" + ((e && e.constructor && e.constructor.name) || "unknown")); }
try { __p("id=" + String((function(){ return 1; })())); } catch (e) { __p("id=!threw:" + ((e && e.constructor && e.constructor.name) || "unknown")); }
