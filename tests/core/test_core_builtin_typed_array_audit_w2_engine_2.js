// covers: c:src/builtins/builtin-typed-array.c
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
const __kitS = __kitSections("test_core_builtin_typed_array_audit_w2_engine_2");
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

__kitS.section("BigInt64Array OOB int-key store of a Number throws", () => {
    const b = new BigInt64Array(1);
    const k = 5;
    throws(() => { b[k] = 1; }, "BigInt64Array OOB int-key store of a Number throws");
    throws(() => { b[k] = 1.5; }, "BigInt64Array OOB int-key store of a double throws");
    throws(() => { b["5"] = 1; }, "BigInt64Array OOB string-key store throws (control)");
    const u = new BigUint64Array(1);
    throws(() => { u[k] = 1; }, "BigUint64Array OOB int-key store of a Number throws");
    const f = new Float64Array(1);
    f[k] = 1;
    eq(f.length, 1, "Float64Array OOB store stays a silent no-op (control)");
});

__kitS.section("Float32Array.sum accumulates in double", () => {
    const n = 1000000, v = Math.fround(0.1);
    const f = new Float32Array(n).fill(0.1);
    ok(Math.abs(f.sum() - n * v) < 1e-6, "Float32Array.sum accumulates in double (got " + f.sum() + ")");
    ok(Math.abs(f.mean() - v) < 1e-12, "Float32Array.mean accumulates in double (got " + f.mean() + ")");
    for (const len of [0, 1, 3, 4, 5, 7, 8, 9, 17]) {
        const t = new Float32Array(len);
        let want = 0;
        for (let i = 0; i < len; i++) { t[i] = i + 0.25; want += t[i]; }
        eq(t.sum(), want, "Float32Array.sum length " + len);
    }
});
__kitS.end();

if (failures) {
    print("test_audit_w2_engine_2: " + failures + " of " + checks + " FAILED");
    throw new Error("test_audit_w2_engine_2 failed");
}
print("test_audit_w2_engine_2: " + checks + " checks passed");
