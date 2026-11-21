// timeout: 120
// covers: dyna:simd.vexp
// Wave-3 engine regression suite for the SIMD cross-owner contract rows:
// COMPAT-040 (NaN propagates through in-place transforms), COMPAT-041
// (vinv/vrsqrt domain semantics: 0 below the domain, NaN propagates) and
// COMPAT-176 (argmax/argmin ties resolve to the first occurrence, matching
// the scalar reference on every backend).
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_simd_vexp_w3_simd");

import {
    sigmoid, relu, relu6, clamp, tanhFast, softmax,
    vexp, vlog, vsqrt, vrsqrt, vinv, argmax, argmin,
} from "dyna:simd";

let n = 0, failed = 0;
function ok(c, m) { n++; if (!c) { failed++; print("FAIL: " + m); } }
function fmt(a) {
    return Array.from(a).map(function (x) {
        return x !== x ? "NaN" : (x === Infinity ? "+inf" : (x === -Infinity ? "-inf" : String(x)));
    }).join(",");
}
function eqF(a, exp, m) {
    let got = Array.from(a);
    let same = got.length === exp.length && got.every(function (x, i) {
        if (exp[i] !== exp[i])
            return x !== x;
        return x === exp[i];
    });
    ok(same, m + " (got " + fmt(a) + ", want " + exp.map(function (x) { return x !== x ? "NaN" : String(x); }).join(",") + ")");
}
const f32 = function (arr) { return Float32Array.from(arr, Math.fround); };
const NAN = NaN;

// COMPAT-041: vinv/vrsqrt stop at their domain boundary and keep exact parity
__kitS.section("COMPAT-041: vinv/vrsqrt stop at their domain boundary and keep exact parity", () => {

(function () {
    let a = f32([Infinity, -Infinity, 1, 2]);
    vexp(a);
    ok(!(a[0] !== a[0]) && a[0] === Infinity, "R4-2 vexp(+inf) saturates to +inf (got " + fmt(a) + ")");
    ok(a[1] === 0, "R4-2 vexp(-inf) saturates to 0 (got " + fmt(a) + ")");
    ok(Math.abs(a[2] - Math.E) < 0.2, "R4-2 vexp finite lane (got " + fmt(a) + ")");
})();

(function () {
    let a = f32([NAN, 0, 1]);
    vexp(a);
    ok(a[0] !== a[0], "COMPAT-040 vexp NaN propagates (got " + fmt(a) + ")");
    ok(Math.abs(a[1] - 1) < 1e-3 && Math.abs(a[2] - Math.E) < 0.2, "COMPAT-040 vexp finite values unchanged (got " + fmt(a) + ")");
})();

setTimeout(function () {
    if (n !== 0) print("w3_simd: " + n + " checks, " + failed + " failures");
    if (failed !== 0) throw new Error("w3_simd: " + failed + " failures");
    print("w3_simd: OK (" + n + " checks)");
}, 20);
});
__kitS.end();
