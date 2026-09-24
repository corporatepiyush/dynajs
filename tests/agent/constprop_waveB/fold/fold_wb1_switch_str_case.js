// fold-active standalone variant of wb1_switch_str_case.js (gen_waveb.py); transcript checked by run_fold.sh
const M = Object.freeze({ S: "go", T: true, I: 42 });
function ds(x) {
    switch (x) {
        case M.S: return "s";
        case M.I: return 1;
        default: return "d";
    }
}
function ds2(x) {
    switch (x) {
        case M.S: return "s";
        case "no": return "n";
        default: return "d2";
    }
}
function __p(s) { if (typeof print === "function") print(s); else console.log(s); }
try { __p("hit_str=" + String((function(){ return ds(M.S); })())); } catch (e) { __p("hit_str=!threw:" + ((e && e.constructor && e.constructor.name) || "unknown")); }
try { __p("hit_int=" + String((function(){ return ds(M.I); })())); } catch (e) { __p("hit_int=!threw:" + ((e && e.constructor && e.constructor.name) || "unknown")); }
try { __p("miss=" + String((function(){ return ds(0); })())); } catch (e) { __p("miss=!threw:" + ((e && e.constructor && e.constructor.name) || "unknown")); }
try { __p("str2=" + String((function(){ return ds2(M.S); })())); } catch (e) { __p("str2=!threw:" + ((e && e.constructor && e.constructor.name) || "unknown")); }
try { __p("miss2=" + String((function(){ return ds2(1); })())); } catch (e) { __p("miss2=!threw:" + ((e && e.constructor && e.constructor.name) || "unknown")); }
