// fold-active standalone variant of wb1_kill_alias_ret.js (gen_waveb.py); transcript checked by run_fold.sh
const M = Object.freeze({ S: "hello", I: 42 });
function id() { return M; } try { id().S = 9; } catch (e) {}
function __p(s) { if (typeof print === "function") print(s); else console.log(s); }
try { __p("post=" + String((function(){ return M.S; })())); } catch (e) { __p("post=!threw:" + ((e && e.constructor && e.constructor.name) || "unknown")); }
try { __p("postI=" + String((function(){ return M.I; })())); } catch (e) { __p("postI=!threw:" + ((e && e.constructor && e.constructor.name) || "unknown")); }
try { __p("still=" + String((function(){ return typeof M; })())); } catch (e) { __p("still=!threw:" + ((e && e.constructor && e.constructor.name) || "unknown")); }
