// timeout: 120
// covers: c:src/runtime/using.inc.c
// Regression suite for the WS-ENGINE audit fixes.
// Each row names the finding ID it pins. Timing rows use generous budgets so
// they discriminate quadratic (multi-second) from linear (millisecond) work
// without flaking on slow hosts.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_core_using_inc_engine_audit_regress");
import { cpuNow } from "../lib/cpuclock.js";

let n = 0, failed = 0;
function ok(c, m) {
    n++;
    if (!c) { failed++; print("FAIL: " + m); }
}
function eq(a, b, m) { ok(JSON.stringify(a) === JSON.stringify(b), m + " (got " + JSON.stringify(a) + ", want " + JSON.stringify(b) + ")"); }
function throwsType(fn, m) {
    let e = null;
    try { fn(); } catch (err) { e = err; }
    ok(e !== null, m + " must throw (got no throw)");
    if (e !== null) ok(e instanceof TypeError, m + " must throw TypeError (got " + e.name + ": " + e.message + ")");
}
function throwsAny(fn, m) {
    let e = null;
    try { fn(); } catch (err) { e = err; }
    ok(e !== null, m + " must throw (got no throw)");
}

// SEC-002: removeRange int64 overflow
__kitS.section("SEC-002: removeRange int64 overflow", () => {

let usingSupported = true;

try { new Function("o", "using x = o;"); } catch (e) { usingSupported = false; }
if (usingSupported) {
    let dispKind = null;
    let o = {};
    o[Symbol.asyncDispose] = function () { dispKind = "async"; return Promise.resolve(); };
    try {
        new Function("o", "using x = o; return 1;")(o);
    } catch (e) {
        dispKind = e instanceof TypeError ? "typeerror" : e.name;
    }
    ok(dispKind === "typeerror", "COMPAT-023 sync using with only asyncDispose must not call it (got " + dispKind + ")");
} else {
    print("COMPAT-023: skipped (using syntax not compiled in)");
}

setTimeout(function () {
    if (n !== 0) print("engine_audit_regress: " + n + " checks, " + failed + " failures");
    if (failed !== 0) throw new Error("engine_audit_regress: " + failed + " failures");
    print("engine_audit_regress: OK (" + n + " checks)");
}, 20);
});
__kitS.end();
