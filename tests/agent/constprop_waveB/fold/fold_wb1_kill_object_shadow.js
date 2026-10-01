var Object2 = Object;
function fake() { return Object2; }
const M = Object.freeze({ S: "hi" });
function __p(s) { if (typeof print === "function") print(s); else console.log(s); }
try { __p("post=" + String((function(){ return M.S; })())); } catch (e) { __p("post=!threw:" + ((e && e.constructor && e.constructor.name) || "unknown")); }
try { __p("objok=" + String((function(){ return typeof fake(); })())); } catch (e) { __p("objok=!threw:" + ((e && e.constructor && e.constructor.name) || "unknown")); }
