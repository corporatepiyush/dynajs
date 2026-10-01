const M = Object.freeze({ T: true, F: false });
function __p(s) { if (typeof print === "function") print(s); else console.log(s); }
try { __p("tmpl0=" + String((function(){ return "t=" + M.T; })())); } catch (e) { __p("tmpl0=!threw:" + ((e && e.constructor && e.constructor.name) || "unknown")); }
try { __p("id=" + String((function(){ return 1; })())); } catch (e) { __p("id=!threw:" + ((e && e.constructor && e.constructor.name) || "unknown")); }
