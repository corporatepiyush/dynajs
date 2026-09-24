// fold-active standalone variant of wb3_labels_mixed.js (gen_waveb.py); transcript checked by run_fold.sh
const M = Object.freeze({ A: 1, B: 2, S: "x" });
let acc = 0;
outer:
for (let i = 0; i < 2; i++) {
    acc += M.A;
    for (let j = 0; j < 3; j++) {
        if (j === 1) { acc += M.S.length; continue outer; }
        acc += M.B;
    }
}
function __p(s) { if (typeof print === "function") print(s); else console.log(s); }
try { __p("acc=" + String((function(){ return acc; })())); } catch (e) { __p("acc=!threw:" + ((e && e.constructor && e.constructor.name) || "unknown")); }
