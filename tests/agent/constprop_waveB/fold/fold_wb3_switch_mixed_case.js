// fold-active standalone variant of wb3_switch_mixed_case.js (gen_waveb.py); transcript checked by run_fold.sh
const M = Object.freeze({ I1: 5, I2: 9, S: "sx" });
function f(x) {
    switch (x) {
        case M.I1: return "i1";
        case M.S: return "s";
        case M.I2: return "i2";
        default: return "d";
    }
}
function __p(s) { if (typeof print === "function") print(s); else console.log(s); }
try { __p("i1=" + String((function(){ return f(M.I1); })())); } catch (e) { __p("i1=!threw:" + ((e && e.constructor && e.constructor.name) || "unknown")); }
try { __p("s=" + String((function(){ return f(M.S); })())); } catch (e) { __p("s=!threw:" + ((e && e.constructor && e.constructor.name) || "unknown")); }
try { __p("i2=" + String((function(){ return f(M.I2); })())); } catch (e) { __p("i2=!threw:" + ((e && e.constructor && e.constructor.name) || "unknown")); }
try { __p("d=" + String((function(){ return f(0); })())); } catch (e) { __p("d=!threw:" + ((e && e.constructor && e.constructor.name) || "unknown")); }
