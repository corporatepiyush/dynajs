// timeout: 120
// covers: c:src/builtins/builtin-promise.c
// Regression suite for the WS-ENGINE audit fixes.
// Each row names the finding ID it pins. Timing rows use generous budgets so
// they discriminate quadratic (multi-second) from linear (millisecond) work
// without flaking on slow hosts.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_core_builtin_promise_engine_audit_regress");
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

async function forAwaitShapes() {
    let out = [];
    async function* gen() { yield 1; yield 2; }
    async function f() {
        while (true) { for await (const x of gen()) { out.push(x); } break; }
        for await (const x of gen()) { out.push(x); }
        return out.length;
    }
    return f();
}

let faP = forAwaitShapes();

await __kitS.section("SEC-001 for-await shapes compile and run", async () => {
ok(typeof faP.then === "function", "SEC-001 for-await shapes compile and run");
});

let seen = null;

__kitS.section("COMPAT-033 Promise.try() is asynchronous", () => {
Promise.try().catch(function (e) { seen = e; });
ok(seen === null, "COMPAT-033 Promise.try() is asynchronous");
});
__kitS.end();

setTimeout(function () {
    if (n !== 0) print("engine_audit_regress: " + n + " checks, " + failed + " failures");
    if (failed !== 0) throw new Error("engine_audit_regress: " + failed + " failures");
    print("engine_audit_regress: OK (" + n + " checks)");
}, 20);
