// fold-active standalone variant of wb2_ok_loop_labels.js (gen_waveb.py); transcript checked by run_fold.sh
function f() {
    const M = Object.freeze({ A: 1, B: 2, S: "x" });
    let acc = 0;
    outer:
    for (let i = 0; i < 3; i++) {
        for (let j = 0; j < 3; j++) {
            if (j === 1) { acc += M.A; continue outer; }
            acc += M.B + M.S.length;
        }
    }
    return acc;
}
function __p(s) { if (typeof print === "function") print(s); else console.log(s); }
try { __p("f=" + String((function(){ return f(); })())); } catch (e) { __p("f=!threw:" + ((e && e.constructor && e.constructor.name) || "unknown")); }
