// timeout: 120
// Wave-3 engine regression suite for the SIMD cross-owner contract rows:
// COMPAT-040 (NaN propagates through in-place transforms), COMPAT-041
// (vinv/vrsqrt domain semantics: 0 below the domain, NaN propagates) and
// COMPAT-176 (argmax/argmin ties resolve to the first occurrence, matching
// the scalar reference on every backend).

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
eqF((function () { let a = f32([-1, 0, 1, -0.5, 2]); vinv(a); return a; })(), [-1, 0, 1, -2, 0.5], "COMPAT-041 vinv domain");
eqF((function () { let a = f32([-1, 0, 1, 4]); vrsqrt(a); return a; })(), [0, 0, 1, 0.5], "COMPAT-041 vrsqrt domain");
eqF((function () { let a = f32([NAN, 0, 1]); vinv(a); return a; })(), [NAN, 0, 1], "COMPAT-041 vinv NaN propagates");
eqF((function () { let a = f32([NAN, 0, 1, 4]); vrsqrt(a); return a; })(), [NAN, 0, 1, 0.5], "COMPAT-041 vrsqrt NaN propagates");
eqF((function () { let a = f32([-1, 0, 4]); vsqrt(a); return a; })(), [0, 0, 2], "COMPAT-041 vsqrt domain (reference)");

// R4-2/R4-3: kernel-reachable rows. The wrapper delegates to the scalar
// reference only when the array holds a NaN (vexp/vlog/vsqrt) or a
// non-positive value (vinv/vrsqrt); the all-positive, NaN-free rows below
// therefore run the active backend's vector kernel (and its scalar tail for
// lengths under the vector width), with expected values taken from the
// scalar reference. x86 SSE42/AVX2/AVX512 kernels are source- and
// syntax-checked only on this arm64 host, so this file pins the active
// backend here (NEON on arm64) and would pin the same rules elsewhere.
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
(function () {
    let a = f32([-0, 1, 4, 9]);
    vsqrt(a);
    ok(Object.is(a[0], -0), "R4-2 vsqrt(-0) keeps the scalar reference sign (got " + fmt(a) + ")");
})();
(function () {
    let a = f32([Infinity, -Infinity, 1, 2]);
    vexp(a);
    ok(!(a[0] !== a[0]) && a[0] === Infinity, "R4-2 vexp(+inf) saturates to +inf (got " + fmt(a) + ")");
    ok(a[1] === 0, "R4-2 vexp(-inf) saturates to 0 (got " + fmt(a) + ")");
    ok(Math.abs(a[2] - Math.E) < 0.2, "R4-2 vexp finite lane (got " + fmt(a) + ")");
})();
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

// COMPAT-040: NaN propagates through every in-place transform
eqF((function () { let a = f32([NAN, -1, 1]); relu(a); return a; })(), [NAN, 0, 1], "COMPAT-040 relu NaN");
eqF((function () { let a = f32([NAN, -1, 10]); relu6(a); return a; })(), [NAN, 0, 6], "COMPAT-040 relu6 NaN");
eqF((function () { let a = f32([NAN, -1, 10]); clamp(a, 0, 6); return a; })(), [NAN, 0, 6], "COMPAT-040 clamp NaN");
eqF((function () { let a = f32([NAN, -40, 0, 40]); sigmoid(a); return a; })(), [NAN, 0, 0.5, 1], "COMPAT-040 sigmoid NaN");
eqF((function () { let a = f32([NAN, -40, 0, 40]); tanhFast(a); return a; })(), [NAN, -1, 0, 1], "COMPAT-040 tanhFast NaN");
// ln(fround(e)) is 0.99999996...: its two neighbouring floats are 0.99999994 and 1, and which
// one a libm returns differs by platform, so that element is held to one ulp, not to a bit pattern.
(function () {
    let a = f32([NAN, -1, 1, Math.E]); vlog(a);
    eqF(a.subarray(0, 3), [NAN, -3.4028234663852886e+38, 0], "COMPAT-040 vlog NaN");
    eqF([Math.abs(a[3] - 1) <= 2 ** -23 ? 1 : a[3]], [1], "COMPAT-040 vlog of e is 1 to one ulp");
})();
eqF((function () { let a = f32([NAN, 0, 4]); vsqrt(a); return a; })(), [NAN, 0, 2], "COMPAT-040 vsqrt NaN");
eqF((function () { let a = f32([NAN, 0, 1]); softmax(a); return a; })(), [NAN, NAN, NAN], "COMPAT-040 softmax NaN");
(function () {
    let a = f32([NAN, 0, 1]);
    vexp(a);
    ok(a[0] !== a[0], "COMPAT-040 vexp NaN propagates (got " + fmt(a) + ")");
    ok(Math.abs(a[1] - 1) < 1e-3 && Math.abs(a[2] - Math.E) < 0.2, "COMPAT-040 vexp finite values unchanged (got " + fmt(a) + ")");
})();

// COMPAT-176: ties resolve to the first occurrence on every backend
ok(argmax(f32([1, 1, 9, 1, 9, 1, 1, 1])) === 2, "COMPAT-176 argmax first-occurrence tie");
ok(argmin(f32([1, 1, -9, 1, -9, 1, 1, 1])) === 2, "COMPAT-176 argmin first-occurrence tie");
ok(argmax(f32([5])) === 0, "COMPAT-176 argmax single element");
ok(argmax(f32([3, 3, 3, 3, 3, 3, 3, 3, 3])) === 0, "COMPAT-176 argmax all-equal");
ok(argmin(f32([2, 1, 1, 1, 1, 1, 1, 1, 1, 1])) === 1, "COMPAT-176 argmin first of a tail run");
(function () {
    let a = new Float32Array(1000);
    for (let i = 0; i < a.length; i++) a[i] = 7;
    ok(argmax(a) === 0 && argmin(a) === 0, "COMPAT-176 argmax/argmin all-equal large");
})();

setTimeout(function () {
    if (n !== 0) print("w3_simd: " + n + " checks, " + failed + " failures");
    if (failed !== 0) throw new Error("w3_simd: " + failed + " failures");
    print("w3_simd: OK (" + n + " checks)");
}, 20);
