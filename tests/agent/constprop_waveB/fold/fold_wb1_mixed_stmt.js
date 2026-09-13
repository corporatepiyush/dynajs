// fold-active standalone variant of wb1_mixed_stmt.js (gen_waveb.py); transcript checked by run_fold.sh
const M = Object.freeze({ S: "hello", W: "\u00e9\u4e2d", C: "7", T: true, F: false, N: null, I: 42 });
function __p(s) { if (typeof print === "function") print(s); else console.log(s); }
try { __p("stmt0=" + String((function(){ return M.S; })())); } catch (e) { __p("stmt0=!threw:" + ((e && e.constructor && e.constructor.name) || "unknown")); }
try { __p("stmt1=" + String((function(){ return M.W; })())); } catch (e) { __p("stmt1=!threw:" + ((e && e.constructor && e.constructor.name) || "unknown")); }
try { __p("stmt2=" + String((function(){ return M.C; })())); } catch (e) { __p("stmt2=!threw:" + ((e && e.constructor && e.constructor.name) || "unknown")); }
try { __p("stmt3=" + String((function(){ return M.T; })())); } catch (e) { __p("stmt3=!threw:" + ((e && e.constructor && e.constructor.name) || "unknown")); }
try { __p("stmt4=" + String((function(){ return M.F; })())); } catch (e) { __p("stmt4=!threw:" + ((e && e.constructor && e.constructor.name) || "unknown")); }
try { __p("stmt5=" + String((function(){ return M.N; })())); } catch (e) { __p("stmt5=!threw:" + ((e && e.constructor && e.constructor.name) || "unknown")); }
try { __p("stmt6=" + String((function(){ return M.I; })())); } catch (e) { __p("stmt6=!threw:" + ((e && e.constructor && e.constructor.name) || "unknown")); }
try { __p("typeof=" + String((function(){ return typeof M.S + typeof M.T + typeof M.N; })())); } catch (e) { __p("typeof=!threw:" + ((e && e.constructor && e.constructor.name) || "unknown")); }
