// covers: c:src/builtins/builtin-typed-array.c
// Split from tests/test_optguide_regressions.js: the core sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_core_builtin_typed_array_optguide_regressions");
export {};

let pass = 0, fail = 0;
function ok(c, m) { if (c) { pass++; } else { fail++; print("FAIL: " + m); } }
function eq(a, b, m) { ok(Object.is(a, b) || a === b, m + "  (got " + a + ", want " + b + ")"); }
function eqj(a, b, m) {
    const x = JSON.stringify(a), y = JSON.stringify(b);
    ok(x === y, m + "  (got " + x + ", want " + y + ")");
}

__kitS.section("BigInt.asUintN matches a mod 2^bits over 160 widths", () => {
    const refU = (bits, a) => { const m = 1n << BigInt(bits); return ((a % m) + m) % m; };
    const refI = (bits, a) => {
        if (bits === 0) return 0n;
        const m = 1n << BigInt(bits), h = 1n << BigInt(bits - 1);
        const v = ((a % m) + m) % m;
        return v >= h ? v - m : v;
    };
    const vals = [-1n, -2n, -12345678901234567890n, 0n, 1n, 255n,
                  1n << 63n, -(1n << 63n), (1n << 64n) - 1n, -(1n << 64n),
                  (1n << 200n) - 1n, -(1n << 200n) - 7n];
    let wrongU = 0, wrongI = 0, neg = 0;
    for (let bits = 0; bits <= 160; bits++) {
        for (const a of vals) {
            const u = BigInt.asUintN(bits, a);
            if (u !== refU(bits, a)) wrongU++;
            if (u < 0n) neg++;
            if (BigInt.asIntN(bits, a) !== refI(bits, a)) wrongI++;
        }
    }
    eq(wrongU, 0, "BigInt.asUintN matches a mod 2^bits over 160 widths");
    eq(neg, 0, "BigInt.asUintN never returns a negative BigInt");
    eq(wrongI, 0, "BigInt.asIntN is unchanged");

    if (typeof [].groupBy === "function") {
        const r = [1, 2].groupBy(() => "__proto__");
        ok(Object.getPrototypeOf(r) === Object.prototype,
           "groupBy: a __proto__ key does not retarget the result's prototype");
        ok(Array.isArray(r.__proto__ === Object.prototype ? r["__proto__"] : null) ||
           Object.hasOwn(r, "__proto__"),
           "groupBy: the __proto__ group is an own property of the result");
        const g = [1, 2, 3].groupBy((x) => (x % 2 ? "odd" : "even"));
        eqj(g.odd, [1, 3], "groupBy still groups normally");
        eqj(g.even, [2], "groupBy even bucket");
    }
});
__kitS.end();
print("test_core_optguide_regressions: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error(fail + " failures");
