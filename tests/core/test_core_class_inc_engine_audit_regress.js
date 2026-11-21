// timeout: 120
// covers: c:src/runtime/class.inc.c
// Regression suite for the WS-ENGINE audit fixes.
// Each row names the finding ID it pins. Timing rows use generous budgets so
// they discriminate quadratic (multi-second) from linear (millisecond) work
// without flaking on slow hosts.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_core_class_inc_engine_audit_regress");
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

eq(new Function("return class C { get\nfoo() { return 7; } };")() .prototype.foo, 7, "COMPAT-022 class getter with line terminator");

eq(new Function("return class C { set\nfoo(v) { this._v = v; } };")() .prototype.set, undefined, "COMPAT-022 class setter with line terminator parses");

let gfield = new Function("return class C { get = 3; };")();

eq(new gfield().get, 3, "COMPAT-022 get as a class field name");

eq(new Function("class C { #x = 1; m() { return this.#x; } }; return new C().m();")(), 1, "COMPAT-117 private name token carries its escape flag");

setTimeout(function () {
    if (n !== 0) print("engine_audit_regress: " + n + " checks, " + failed + " failures");
    if (failed !== 0) throw new Error("engine_audit_regress: " + failed + " failures");
    print("engine_audit_regress: OK (" + n + " checks)");
}, 20);
});
__kitS.end();
