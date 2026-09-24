// fold-active standalone variant of wb3_multiline.js (gen_waveb.py); transcript checked by run_fold.sh
const M = Object.freeze({
    A: 11,
    S: "line"
});
function f() {
    let x = M.
A;
    let y = M[
"S"];
    return x + ":" + y;
}
function __p(s) { if (typeof print === "function") print(s); else console.log(s); }
try { __p("f=" + String((function(){ return f(); })())); } catch (e) { __p("f=!threw:" + ((e && e.constructor && e.constructor.name) || "unknown")); }
try { __p("line=" + String((function(){ return 1; })())); } catch (e) { __p("line=!threw:" + ((e && e.constructor && e.constructor.name) || "unknown")); }
