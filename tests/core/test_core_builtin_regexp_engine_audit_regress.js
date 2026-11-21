// timeout: 120
// covers: c:src/builtins/builtin-regexp.c
// Regression suite for the WS-ENGINE audit fixes.
// Each row names the finding ID it pins. Timing rows use generous budgets so
// they discriminate quadratic (multi-second) from linear (millisecond) work
// without flaking on slow hosts.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_core_builtin_regexp_engine_audit_regress");
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

(function () {
    let t0 = cpuNow();
    let re = new RegExp("a|".repeat(200000) + "a");
    let dt = cpuNow() - t0;
    ok(dt < 4000, "SEC-023 alternation compile stays linear (" + dt + "ms for 200k alternatives)");
    ok(re.test("a") && re.test("b") === false, "SEC-023 alternation still matches correctly");
    t0 = cpuNow();
    let re2 = new RegExp("\\k<x>".repeat(50000) + "(?<x>a)");
    dt = cpuNow() - t0;
    ok(dt < 4000, "SEC-023 forward named backref compile stays linear (" + dt + "ms)");
    ok(re2.test("aa"), "SEC-023 forward named backref still matches (aa)");
})();

eq(/(?<x>a)\k<x>/.exec("aa")[0], "aa", "SEC-023 backward named backref");

throwsAny(function () { new RegExp("(?<x>a)(?<x>b)"); }, "SEC-023 duplicate group name refused");

throwsAny(function () { new RegExp("\\k<nope>", "u"); }, "SEC-023 undefined forward name refused (unicode)");

let multiAlt = new RegExp("(a)|(b)|(c)");

eq(multiAlt.exec("b")[0], "b", "SEC-023 three-alternative priority");

eq([...("abc".matchAll(new RegExp("a|b|c", "g")))].map(function (m) { return m[0]; }), ["a", "b", "c"], "SEC-023 matchAll alternation");

ok(new RegExp("[\\uDC00-\\uDFFF]", "u").exec("\uD800\uDC00" + "x".repeat(40)) === null, "COMPAT-035 low-surrogate class does not match inside a pair");

eq(new RegExp("[\\uDC00-\\uDFFF]", "u").exec("x\uDC00")[0], "\uDC00", "COMPAT-035 lone low surrogate still matches");

ok((1e300).format(0).replace(/,/g, "").indexOf("100000000000000005250476025520442024870446858110815915491585411551180245798890819578637137508044786404370444383288387817694") === 0, "COMPAT-030 format exact integer digits");

(function () {
    let buf = new ArrayBuffer(8), dv = new DataView(buf);
    let state = 0x123456789abcdefn, bad = 0, longer = 0, checked = 0;
    for (let i = 0; i < 20000; i++) {
        state ^= state << 13n; state &= 0xffffffffffffffffn;
        state ^= state >> 7n;
        state ^= state << 17n; state &= 0xffffffffffffffffn;
        dv.setBigUint64(0, state, false);
        let x = dv.getFloat64(0, false);
        if (!Number.isFinite(x) || x === 0) continue;
        checked++;
        let s = String(x);
        if (parseFloat(s) !== x) bad++;
        let digits = s.replace(/^[-+]/, "").split(/e/i)[0].replace(".", "").replace(/^0+/, "").replace(/0+$/, "");
        if (digits.length > 17) longer++;
    }
    ok(bad === 0, "COMPAT-055 dtoa round-trip on " + checked + " random doubles");
    ok(longer === 0, "COMPAT-055 dtoa uses at most 17 significant digits");
})();

setTimeout(function () {
    if (n !== 0) print("engine_audit_regress: " + n + " checks, " + failed + " failures");
    if (failed !== 0) throw new Error("engine_audit_regress: " + failed + " failures");
    print("engine_audit_regress: OK (" + n + " checks)");
}, 20);
});
__kitS.end();
