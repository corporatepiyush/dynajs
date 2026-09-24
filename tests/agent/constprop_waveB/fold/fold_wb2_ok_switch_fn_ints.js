// fold-active standalone variant of wb2_ok_switch_fn_ints.js (gen_waveb.py); transcript checked by run_fold.sh
function ds(x) {
    const OP = Object.freeze({ K0: 1, K1: 7, K2: 13 });
    switch (x) {
        case OP.K0: return "a";
        case OP.K1: return "b";
        case OP.K2: return "c";
        default: return "d";
    }
}
function __p(s) { if (typeof print === "function") print(s); else console.log(s); }
try { __p("h0=" + String((function(){ return ds(1); })())); } catch (e) { __p("h0=!threw:" + ((e && e.constructor && e.constructor.name) || "unknown")); }
try { __p("h1=" + String((function(){ return ds(7); })())); } catch (e) { __p("h1=!threw:" + ((e && e.constructor && e.constructor.name) || "unknown")); }
try { __p("h2=" + String((function(){ return ds(13); })())); } catch (e) { __p("h2=!threw:" + ((e && e.constructor && e.constructor.name) || "unknown")); }
try { __p("hm=" + String((function(){ return ds(5); })())); } catch (e) { __p("hm=!threw:" + ((e && e.constructor && e.constructor.name) || "unknown")); }
