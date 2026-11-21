// flags: --std
// covers: c:src/vm/interpreter.inc.c
// Split from tests/test_audit_p1.js: the core sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_core_interpreter_inc_audit_p1");
import * as os from "os";

let n = 0, fails = 0;
function assert(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { n++; if (a !== b) { fails++; print("FAIL: " + m + " got " + JSON.stringify(a) + " want " + JSON.stringify(b)); } }
function throws(fn, re, m) {
    n++;
    try { fn(); fails++; print("FAIL: " + m + " did not throw"); }
    catch (e) {
        if (re && !re.test(String(e.message))) { fails++; print("FAIL: " + m + " wrong message: " + e.message); }
    }
}

__kitS.section("setTimeout trailing args covered by test_fn_timers", () => {
assert(true, "setTimeout trailing args covered by test_fn_timers");
if (fails) { print("test_core_audit_p1: " + fails + " FAILED of " + n + " assertions"); throw new Error("test_core_audit_p1 failed"); }
print("test_core_audit_p1: " + n + " assertions, 0 failures");
});
__kitS.end();
