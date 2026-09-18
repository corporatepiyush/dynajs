// fold-active standalone variant of wb1_bool_eq.js (gen_waveb.py); transcript checked by run_fold.sh
const M = Object.freeze({ T: true, F: false });
function __p(s) { if (typeof print === "function") print(s); else console.log(s); }
try { __p("eq0=" + String((function(){ return (M.T === M.T); })())); } catch (e) { __p("eq0=!threw:" + ((e && e.constructor && e.constructor.name) || "unknown")); }
try { __p("id=" + String((function(){ return 1; })())); } catch (e) { __p("id=!threw:" + ((e && e.constructor && e.constructor.name) || "unknown")); }
