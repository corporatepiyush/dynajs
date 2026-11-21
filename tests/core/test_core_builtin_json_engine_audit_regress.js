// timeout: 120
// covers: c:src/builtins/builtin-json.c
// Regression suite for the WS-ENGINE audit fixes.
// Each row names the finding ID it pins. Timing rows use generous budgets so
// they discriminate quadratic (multi-second) from linear (millisecond) work
// without flaking on slow hosts.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_core_builtin_json_engine_audit_regress");
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

eq(JSON.stringify({ a: JSON.rawJSON("true") }), '{"a":true}', "COMPAT-032 rawJSON true");

eq(JSON.stringify({ a: JSON.rawJSON("null") }), '{"a":null}', "COMPAT-032 rawJSON null");

eq(JSON.stringify({ a: JSON.rawJSON("1e3") }), '{"a":1e3}', "COMPAT-032 rawJSON exponent number");

eq(JSON.stringify({ a: JSON.rawJSON('"x"') }), '{"a":"x"}', "COMPAT-032 rawJSON string");

throwsAny(function () { JSON.rawJSON('{"a":1}'); }, "COMPAT-032 rawJSON object text refused");

throwsAny(function () { JSON.rawJSON("[1]"); }, "COMPAT-032 rawJSON array text refused");

throwsAny(function () { JSON.rawJSON(""); }, "COMPAT-032 rawJSON empty refused");

eq(JSON.parse(JSON.stringify("\u0080\u00ff")), "\u0080\u00ff", "COMPAT-153 JSON round-trip high Latin1");

setTimeout(function () {
    if (n !== 0) print("engine_audit_regress: " + n + " checks, " + failed + " failures");
    if (failed !== 0) throw new Error("engine_audit_regress: " + failed + " failures");
    print("engine_audit_regress: OK (" + n + " checks)");
}, 20);
});
__kitS.end();
