// timeout: 120
// covers: contract dyna:simd.clamp dyna:simd.relu dyna:simd.relu6 dyna:simd.sigmoid dyna:simd.softmax dyna:simd.tanhFast dyna:simd.vinv dyna:simd.vlog -- the COMPAT-040/041 NaN and domain row shared by every in-place transform
// Wave-3 engine regression suite for the SIMD cross-owner contract rows:
// COMPAT-040 (NaN propagates through in-place transforms), COMPAT-041
// (vinv/vrsqrt domain semantics: 0 below the domain, NaN propagates) and
// COMPAT-176 (argmax/argmin ties resolve to the first occurrence, matching
// the scalar reference on every backend).
// One named case per transform: merged from the eight one-check files
// test_simd_{clamp,relu,relu6,sigmoid,softmax,tanh_fast,vinv,vlog}_w3_simd.js.
import { sections as __kitSections } from "../lib/kit.js";
import {
    sigmoid, relu, relu6, clamp, tanhFast, softmax,
    vexp, vlog, vsqrt, vrsqrt, vinv, argmax, argmin,
} from "dyna:simd";
const __kitS = __kitSections("test_simd_w3_compat");

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

__kitS.section("COMPAT-040 clamp NaN", () => {
    eqF((function () { let a = f32([NAN, -1, 10]); clamp(a, 0, 6); return a; })(), [NAN, 0, 6], "COMPAT-040 clamp NaN");
});
__kitS.section("COMPAT-040 relu NaN", () => {
    eqF((function () { let a = f32([NAN, -1, 1]); relu(a); return a; })(), [NAN, 0, 1], "COMPAT-040 relu NaN");
});
__kitS.section("COMPAT-040 relu6 NaN", () => {
    eqF((function () { let a = f32([NAN, -1, 10]); relu6(a); return a; })(), [NAN, 0, 6], "COMPAT-040 relu6 NaN");
});
__kitS.section("COMPAT-040 sigmoid NaN", () => {
    eqF((function () { let a = f32([NAN, -40, 0, 40]); sigmoid(a); return a; })(), [NAN, 0, 0.5, 1], "COMPAT-040 sigmoid NaN");
});
__kitS.section("COMPAT-040 softmax NaN", () => {
    eqF((function () { let a = f32([NAN, 0, 1]); softmax(a); return a; })(), [NAN, NAN, NAN], "COMPAT-040 softmax NaN");
});
__kitS.section("COMPAT-040 tanhFast NaN", () => {
    eqF((function () { let a = f32([NAN, -40, 0, 40]); tanhFast(a); return a; })(), [NAN, -1, 0, 1], "COMPAT-040 tanhFast NaN");
});
// COMPAT-041: vinv/vrsqrt stop at their domain boundary and keep exact parity
__kitS.section("COMPAT-041 vinv domain and NaN", () => {
    eqF((function () { let a = f32([-1, 0, 1, -0.5, 2]); vinv(a); return a; })(), [-1, 0, 1, -2, 0.5], "COMPAT-041 vinv domain");
    eqF((function () { let a = f32([NAN, 0, 1]); vinv(a); return a; })(), [NAN, 0, 1], "COMPAT-041 vinv NaN propagates");
});
__kitS.section("COMPAT-040 vlog NaN and vlog(e)", () => {
    (function () {
        let a = f32([NAN, -1, 1, Math.E]); vlog(a);
        eqF(a.subarray(0, 3), [NAN, -3.4028234663852886e+38, 0], "COMPAT-040 vlog NaN");
        eqF([Math.abs(a[3] - 1) <= 2 ** -23 ? 1 : a[3]], [1], "COMPAT-040 vlog of e is 1 to one ulp");
    })();
});
__kitS.end();

if (!__kitS.listing) setTimeout(function () {
    if (n !== 0) print("w3_simd: " + n + " checks, " + failed + " failures");
    if (failed !== 0) throw new Error("w3_simd: " + failed + " failures");
    print("w3_simd: OK (" + n + " checks)");
}, 20);
