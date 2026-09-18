// fold-active standalone variant of wb1_kill_with.js (gen_waveb.py); transcript checked by run_fold.sh
const M = Object.freeze({ S: "hello", I: 7 });
with ({}) { }
function __p(s) { if (typeof print === "function") print(s); else console.log(s); }
try { __p("post=" + String((function(){ return M.S + M.I; })())); } catch (e) { __p("post=!threw:" + ((e && e.constructor && e.constructor.name) || "unknown")); }
