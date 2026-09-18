// fold-active standalone variant of wb3_try_finally.js (gen_waveb.py); transcript checked by run_fold.sh
const M = Object.freeze({ A: 3, S: "yy" });
function f() {
    let r = M.A;
    try { r += M.S.length; throw new Error('x'); }
    catch (e) { r += M.A; } finally { r += M.A; }
    return r;
}
function __p(s) { if (typeof print === "function") print(s); else console.log(s); }
try { __p("f=" + String((function(){ return f(); })())); } catch (e) { __p("f=!threw:" + ((e && e.constructor && e.constructor.name) || "unknown")); }
