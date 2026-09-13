// fold-active standalone variant of wb1_tdz_early_fn.js (gen_waveb.py); transcript checked by run_fold.sh

var tdz = "no";
function early() { return M.S; }
try { tdz = String(early()); } catch (e) { tdz = (e && e.constructor && e.constructor.name) || "unknown"; }
const M = Object.freeze({ S: "hello" });
function __p(s) { if (typeof print === "function") print(s); else console.log(s); }
try { __p("tdz=" + String((function(){ return tdz; })())); } catch (e) { __p("tdz=!threw:" + ((e && e.constructor && e.constructor.name) || "unknown")); }
try { __p("after=" + String((function(){ return M.S; })())); } catch (e) { __p("after=!threw:" + ((e && e.constructor && e.constructor.name) || "unknown")); }
