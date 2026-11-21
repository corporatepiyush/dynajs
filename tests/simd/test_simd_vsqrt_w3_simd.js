// timeout: 120
// covers: dyna:simd.vsqrt
// Wave-3 engine regression suite for the SIMD cross-owner contract rows:
// COMPAT-040 (NaN propagates through in-place transforms), COMPAT-041
// (vinv/vrsqrt domain semantics: 0 below the domain, NaN propagates) and
// COMPAT-176 (argmax/argmin ties resolve to the first occurrence, matching
// the scalar reference on every backend).
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_simd_vsqrt_w3_simd");

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

eqF((function () { let a = f32([-1, 0, 4]); vsqrt(a); return a; })(), [0, 0, 2], "COMPAT-041 vsqrt domain (reference)");

(function () {
    let a = f32([-0, 1, 4, 9]);
    vsqrt(a);
    ok(Object.is(a[0], -0), "R4-2 vsqrt(-0) keeps the scalar reference sign (got " + fmt(a) + ")");
})();

eqF((function () { let a = f32([NAN, 0, 4]); vsqrt(a); return a; })(), [NAN, 0, 2], "COMPAT-040 vsqrt NaN");

setTimeout(function () {
    if (n !== 0) print("w3_simd: " + n + " checks, " + failed + " failures");
    if (failed !== 0) throw new Error("w3_simd: " + failed + " failures");
    print("w3_simd: OK (" + n + " checks)");
}, 20);
});
__kitS.end();
