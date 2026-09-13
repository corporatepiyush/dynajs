// fold-active standalone variant of wb3_adjacent_args.js (gen_waveb.py); transcript checked by run_fold.sh
const M = Object.freeze({ A: 1, B: 2, S: "z" });
function sum() { let t = M.A; for (var i = 0; i < arguments.length; i++) t += arguments[i]; return t; }
function __p(s) { if (typeof print === "function") print(s); else console.log(s); }
try { __p("s=" + String((function(){ return sum(M.A, M.B, M.S); })())); } catch (e) { __p("s=!threw:" + ((e && e.constructor && e.constructor.name) || "unknown")); }
