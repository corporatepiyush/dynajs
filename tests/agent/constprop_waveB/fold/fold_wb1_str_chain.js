// fold-active standalone variant of wb1_str_chain.js (gen_waveb.py); transcript checked by run_fold.sh
const M = Object.freeze({ S: "hello" });
function __p(s) { if (typeof print === "function") print(s); else console.log(s); }
try { __p("chain0=" + String((function(){ return M.S.length; })())); } catch (e) { __p("chain0=!threw:" + ((e && e.constructor && e.constructor.name) || "unknown")); }
try { __p("id=" + String((function(){ return 1; })())); } catch (e) { __p("id=!threw:" + ((e && e.constructor && e.constructor.name) || "unknown")); }
