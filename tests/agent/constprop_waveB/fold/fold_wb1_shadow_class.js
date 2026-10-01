const M = Object.freeze({ S: "hello" });
const K = class M { };
function __p(s) { if (typeof print === "function") print(s); else console.log(s); }
try { __p("class=" + String((function(){ return M.S; })())); } catch (e) { __p("class=!threw:" + ((e && e.constructor && e.constructor.name) || "unknown")); }
try { __p("top=" + String((function(){ return M.S; })())); } catch (e) { __p("top=!threw:" + ((e && e.constructor && e.constructor.name) || "unknown")); }
