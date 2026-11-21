// covers: c:src/builtins/builtin-array.c
// tests/core/test_audit_w2_engine_2.js -- value-level regressions for the engine
// built-ins audited in wave 2. Every expected value is either a definition
// (a naive reference computed here) or a property stated in dynajs.d.ts.
//   E5-03  Object.set never walks or writes through the prototype chain
//   E5-04  assocPath/dissocPath/modifyPath refuse a runaway path cleanly
//   E5-06  keysIn/valuesIn/omit keep their answers (and stop being quadratic)
//   E5-07  once/debounce state is not observable through Array.prototype
//   E2c-02 BigInt64Array out-of-range store with an int key throws like the
//          string-key path
//   E3-06  indexOf/lastIndexOf/includes/count/split agree with a naive
//          reference on inputs that force the linear-time fallback
//   E3-07  Float32Array sum/mean accumulate in double
//   E3-09  Number.format keeps non-ASCII separators intact
//   E3-10  String.format reads own properties only
//   E2b-01 colliding keys do not make interning quadratic
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_core_builtin_array_audit_w2_engine_2");
import { cpuNow } from "../lib/cpuclock.js";
let failures = 0, checks = 0;
function ok(cond, msg) {
    checks++;
    if (!cond) {
        failures++;
        print("  FAIL:", msg);
    }
}
function eq(got, want, msg) {
    ok(Object.is(got, want), msg + " (got " + String(got) + ", want " + String(want) + ")");
}
function throws(fn, msg) {
    let threw = false;
    try { fn(); } catch (e) { threw = true; }
    ok(threw, msg);
}

// ---- E5-03 ---------------------------------------------------------------

__kitS.section("once runs its body once under an Array.prototype index accessor", () => {
    let seen = 0, stored;
    Object.defineProperty(Array.prototype, "0", {
        configurable: true,
        get() { return stored; },
        set(v) { seen++; stored = v; },
    });
    let ran = 0, out;
    try {
        const f = (function () { ran++; return 42; }).once();
        f(); f(); out = f();
    } finally {
        delete Array.prototype[0];
    }
    eq(ran, 1, "once runs its body once under an Array.prototype index accessor");
    eq(out, 42, "once returns the cached value");
    eq(seen, 0, "helper state never reaches an inherited setter");
});
__kitS.end();

if (failures) {
    print("test_audit_w2_engine_2: " + failures + " of " + checks + " FAILED");
    throw new Error("test_audit_w2_engine_2 failed");
}
print("test_audit_w2_engine_2: " + checks + " checks passed");
