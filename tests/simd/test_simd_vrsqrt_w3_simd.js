// timeout: 120
// covers: dyna:simd.vrsqrt
// Wave-3 engine regression suite for the SIMD cross-owner contract rows:
// COMPAT-040 (NaN propagates through in-place transforms), COMPAT-041
// (vinv/vrsqrt domain semantics: 0 below the domain, NaN propagates) and
// COMPAT-176 (argmax/argmin ties resolve to the first occurrence, matching
// the scalar reference on every backend).
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_simd_vrsqrt_w3_simd");

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

eqF((function () { let a = f32([-1, 0, 1, 4]); vrsqrt(a); return a; })(), [0, 0, 1, 0.5], "COMPAT-041 vrsqrt domain");

eqF((function () { let a = f32([NAN, 0, 1, 4]); vrsqrt(a); return a; })(), [NAN, 0, 1, 0.5], "COMPAT-041 vrsqrt NaN propagates");

function nearF(a, exp, tol, m) {
    let got = Array.from(a);
    let same = got.length === exp.length && got.every(function (x, i) {
        if (exp[i] !== exp[i])
            return x !== x;
        if (exp[i] === Infinity || exp[i] === -Infinity)
            return x === exp[i];
        return Math.abs(x - exp[i]) <= tol * Math.abs(exp[i]);
    });
    ok(same, m + " (got " + fmt(a) + ", want ~" + exp.map(String).join(",") + ")");
}

nearF((function () { let a = f32([Infinity, 1, 2, 3]); vrsqrt(a); return a; })(),
    [0, 1, 1 / Math.sqrt(2), 1 / Math.sqrt(3)], 1e-6, "R4-2 vrsqrt(+inf) vector kernel is 0");
nearF((function () { let a = f32([Infinity, 1, 4, 9, 16, 25, 36, 49, 64]); vrsqrt(a); return a; })(),
    [0, 1, 0.5, 1 / 3, 0.25, 0.2, 1 / 6, 1 / 7, 0.125], 1e-6, "R4-2 vrsqrt 9-lane +inf mix");
nearF((function () { let a = f32([Infinity, 2, 4, 8]); vinv(a); return a; })(),
    [0, 0.5, 0.25, 0.125], 1e-6, "R4-2 vinv(+inf) vector kernel");
eqF((function () { let a = f32([Infinity, 1, 4, 9]); vsqrt(a); return a; })(), [Infinity, 1, 2, 3], "R4-2 vsqrt(+inf) vector kernel");

__kitS.section("R4-2 vrsqrt len", () => {
(function () {
    // Kernel and scalar-tail lengths must agree on the +inf domain.
    for (let len = 1; len <= 9; len++) {
        let v = [Infinity];
        for (let i = 1; i < len; i++) v.push(i * i);
        let a = f32(v);
        vrsqrt(a);
        ok(a[0] === 0, "R4-2 vrsqrt len=" + len + " (+inf lane is 0, got " + fmt(a) + ")");
    }
})();
});
__kitS.end();

setTimeout(function () {
    if (n !== 0) print("w3_simd: " + n + " checks, " + failed + " failures");
    if (failed !== 0) throw new Error("w3_simd: " + failed + " failures");
    print("w3_simd: OK (" + n + " checks)");
}, 20);
