async function f() { const M = Object.freeze({ A: 6 }); return M.A; }
function __p(s) { if (typeof print === "function") print(s); else console.log(s); }
try { __p("t=" + String((function(){ return typeof f(); })())); } catch (e) { __p("t=!threw:" + ((e && e.constructor && e.constructor.name) || "unknown")); }
