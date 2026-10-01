const M = Object.freeze({ S: "hello" });
function f() { let M = { S: "shadow" }; return M.S; }
function __p(s) { if (typeof print === "function") print(s); else console.log(s); }
try { __p("let=" + String((function(){ return f(); })())); } catch (e) { __p("let=!threw:" + ((e && e.constructor && e.constructor.name) || "unknown")); }
try { __p("top=" + String((function(){ return M.S; })())); } catch (e) { __p("top=!threw:" + ((e && e.constructor && e.constructor.name) || "unknown")); }
