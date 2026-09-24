// fold-active standalone variant of wb2_ok_kernel.js (gen_waveb.py); transcript checked by run_fold.sh
function run(p) {
    const OP = Object.freeze({ PUSH: 0, ADD: 1, SUB: 2, HALT: 3 });
    let sp = -1, st = [], pc = 0;
    while (pc < p.length) {
        const op = p[pc++];
        switch (op) {
            case OP.PUSH: st[++sp] = p[pc++]; break;
            case OP.ADD: { const b = st[sp--], a = st[sp--]; st[++sp] = a + b; break; }
            case OP.SUB: { const b = st[sp--], a = st[sp--]; st[++sp] = a - b; break; }
            case OP.HALT: return st[sp];
            default: return NaN;
        }
    }
    return NaN;
}
var prog = [0, 10, 0, 4, 1, 0, 2, 3];
function __p(s) { if (typeof print === "function") print(s); else console.log(s); }
try { __p("run=" + String((function(){ return run(prog); })())); } catch (e) { __p("run=!threw:" + ((e && e.constructor && e.constructor.name) || "unknown")); }
try { __p("again=" + String((function(){ return run(prog); })())); } catch (e) { __p("again=!threw:" + ((e && e.constructor && e.constructor.name) || "unknown")); }
