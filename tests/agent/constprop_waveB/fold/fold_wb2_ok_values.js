// fold-active standalone variant of wb2_ok_values.js (gen_waveb.py); transcript checked by run_fold.sh
function f() {
    const M = Object.freeze({ S: "ss", T: true, N: null, I: -5 });
    return M.S + ":" + (M.T === true) + ":" + (M.N === null) + ":" + (M.I < 0);
}
function __p(s) { if (typeof print === "function") print(s); else console.log(s); }
try { __p("f=" + String((function(){ return f(); })())); } catch (e) { __p("f=!threw:" + ((e && e.constructor && e.constructor.name) || "unknown")); }
try { __p("f2=" + String((function(){ return f(); })())); } catch (e) { __p("f2=!threw:" + ((e && e.constructor && e.constructor.name) || "unknown")); }
