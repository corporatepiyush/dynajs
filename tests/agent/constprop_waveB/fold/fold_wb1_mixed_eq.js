// fold-active standalone variant of wb1_mixed_eq.js (gen_waveb.py); transcript checked by run_fold.sh
const M = Object.freeze({ S: "hello", W: "\u00e9\u4e2d", C: "7", T: true, F: false, N: null, I: 42 });
function __p(s) { if (typeof print === "function") print(s); else console.log(s); }
try { __p("eq0=" + String((function(){ return (M.I === M.I); })())); } catch (e) { __p("eq0=!threw:" + ((e && e.constructor && e.constructor.name) || "unknown")); }
try { __p("typeof=" + String((function(){ return typeof M.S + typeof M.T + typeof M.N; })())); } catch (e) { __p("typeof=!threw:" + ((e && e.constructor && e.constructor.name) || "unknown")); }
