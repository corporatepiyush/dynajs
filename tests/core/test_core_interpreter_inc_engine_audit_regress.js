// timeout: 120
// covers: c:src/vm/interpreter.inc.c
// Regression suite for the WS-ENGINE audit fixes.
// Each row names the finding ID it pins. Timing rows use generous budgets so
// they discriminate quadratic (multi-second) from linear (millisecond) work
// without flaking on slow hosts.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_core_interpreter_inc_engine_audit_regress");
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

__kitS.section("SEC-002 removeRange huge count clamps to empty range", () => {
ok([1, 2, 3].removeRange(3, 0x7fffffffffffffff).length === 3, "SEC-002 removeRange huge count clamps to empty range");
});

__kitS.section("SEC-002 removeRange huge count from 0 removes all", () => {
eq([1, 2, 3].removeRange(0, 0x40000000000000), [], "SEC-002 removeRange huge count from 0 removes all");
});

__kitS.section("SEC-002 removeRange huge count keeps prefix", () => {
eq([1, 2, 3].removeRange(1, 0x7fffffffffffffff), [1], "SEC-002 removeRange huge count keeps prefix");
});

__kitS.section("SEC-002 removeRange normal path", () => {
eq([1, 2, 3, 4].removeRange(1, 2), [1, 4], "SEC-002 removeRange normal path");
});

__kitS.section("SEC-002 removeRange huge negative start clamps to 0", () => {
eq([1, 2, 3].removeRange(-1e30, 2), [3], "SEC-002 removeRange huge negative start clamps to 0");
});

function bigSwitch(x) {
    switch (x) {
    case 2147483647: return 1;
    case -2147483648: return 2;
    case 0: return 3;
    case 1000: return 4;
    case -1000: return 5;
    default: return 0;
    }
}

__kitS.section("SEC-160 switch INT32_MAX", () => {
eq(bigSwitch(2147483647), 1, "SEC-160 switch INT32_MAX");
});

__kitS.section("SEC-160 switch INT32_MIN", () => {
eq(bigSwitch(-2147483648), 2, "SEC-160 switch INT32_MIN");
});

__kitS.section("SEC-160 switch dense label", () => {
eq(bigSwitch(1000), 4, "SEC-160 switch dense label");
});

__kitS.section("COMPAT-030 format(1e300,20) is not truncated", () => {
ok((1e300).format(20).length > 300, "COMPAT-030 format(1e300,20) is not truncated");
});

__kitS.section("COMPAT-030 abbr is not truncated", () => {
ok((1e300).abbr(20).length > 100, "COMPAT-030 abbr is not truncated");
});

__kitS.section("COMPAT-138 round(-400) of small value is 0", () => {
eq((5).round(-400), 0, "COMPAT-138 round(-400) of small value is 0");
});

__kitS.section("COMPAT-138 normal precision unaffected", () => {
eq((1.2345).round(2), 1.23, "COMPAT-138 normal precision unaffected");
});

__kitS.section("COMPAT-138 floor with precision", () => {
eq((-1.2345).floor(2), -1.24, "COMPAT-138 floor with precision");
});

__kitS.section("COMPAT-139 pad clamps place to 65536", () => {
eq((7).pad(600000).length, 65536, "COMPAT-139 pad clamps place to 65536");
});

__kitS.section("COMPAT-139 pad(70000) is 65536 chars", () => {
eq((1234567).pad(70000).length, 65536, "COMPAT-139 pad(70000) is 65536 chars");
});

__kitS.section("COMPAT-139 pad never truncates digits", () => {
eq((1234567).pad(1), "1234567", "COMPAT-139 pad never truncates digits");
});

__kitS.section("COMPAT-139 hex clamps place to 65536", () => {
eq((255).hex(70000).length, 65536, "COMPAT-139 hex clamps place to 65536");
});

__kitS.section("COMPAT-133 nbsp maps to U+00A0", () => {
eq("&nbsp;".unescapeHTML(), "\u00A0", "COMPAT-133 nbsp maps to U+00A0");
});

__kitS.section("SEC-177 splitN normal", () => {
eq("a,b,c".splitN(",", 2), ["a", "b,c"], "SEC-177 splitN normal");
});

__kitS.section("SEC-177 splitN full", () => {
eq("a,b,c".splitN(",", 9), ["a", "b", "c"], "SEC-177 splitN full");
});

__kitS.section("SEC-177 splitN empty", () => {
eq("".splitN(",", 3), [""], "SEC-177 splitN empty");
});

__kitS.section("SEC-180 ordinalize INT64_MIN", () => {
eq((-9223372036854775808).ordinalize(), "-9223372036854775808th", "SEC-180 ordinalize INT64_MIN");
});

__kitS.section("COMPAT-029 removeRange(from,count)", () => {
eq([0, 1, 2, 3, 4, 5].removeRange(1, 2), [0, 3, 4, 5], "COMPAT-029 removeRange(from,count)");
});
__kitS.end();

setTimeout(function () {
    if (n !== 0) print("engine_audit_regress: " + n + " checks, " + failed + " failures");
    if (failed !== 0) throw new Error("engine_audit_regress: " + failed + " failures");
    print("engine_audit_regress: OK (" + n + " checks)");
}, 20);
