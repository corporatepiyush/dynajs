function f() { const M = Object.freeze({ A: 1 }); return M.A; }
function g() { const M = Object.freeze({ A: 2 }); return M.A; }
function __p(s) { if (typeof print === "function") print(s); else console.log(s); }
try { __p("fg=" + String((function(){ return f() + "|" + g(); })())); } catch (e) { __p("fg=!threw:" + ((e && e.constructor && e.constructor.name) || "unknown")); }
