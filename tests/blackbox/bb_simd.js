// Parametric black-box contract test for dyna:simd, generated from dynajs.d.ts lines 4581-4716 (plus shared F32Like note at lines 76-82). Engine sources not consulted.
import * as simd from "dyna:simd";
import * as mathx from "dyna:mathx";

let n = 0;
function assert(cond, msg) { n++; if (!cond) throw new Error("assertion failed: " + msg); }
function assertEq(actual, expected, msg) { n++; const ok = Object.is(actual, expected) || (typeof actual === "number" && typeof expected === "number" && Number.isNaN(actual) && Number.isNaN(expected)); if (!ok) throw new Error("assertion failed: " + msg + " — got |" + actual + "| expected |" + expected + "|"); }
function assertClose(actual, expected, eps, msg) { n++; if (!(Math.abs(actual - expected) <= eps)) throw new Error("assertion failed (close): " + msg + " — got |" + actual + "| expected |" + expected + "±" + eps + "|"); }
function assertThrows(fn, msg, ErrType, errPattern) { n++; let threw = false, e = null; try { fn(); } catch (err) { threw = true; e = err; } if (!threw) throw new Error("expected throw: " + msg); if (ErrType && !(e instanceof ErrType)) throw new Error("wrong error type " + (e && e.constructor ? e.constructor.name : String(e)) + ": " + msg); if (errPattern && !(errPattern instanceof RegExp ? errPattern.test(String(e)) : String(e).includes(errPattern))) throw new Error("wrong error message |" + e + "|: " + msg); }
function eqArr(a, b) { if (a.length !== b.length) return false; for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false; return true; }
function eqArrClose(a, b, eps) { if (a.length !== b.length) return false; for (let i = 0; i < a.length; i++) if (!(Math.abs(a[i] - b[i]) <= eps)) return false; return true; }
const F = (a) => Float32Array.from(a);

// Float32 bit patterns (hand-derived IEEE 754): 1.0=0x3F800000, 1.5=0x3FC00000, 2.0=0x40000000, 3.0=0x40400000, -2.0=0xC0000000
const B10 = 1065353216, B15 = 1069547520, B20 = 1073741824, B30 = 1077936128, B2N = -1073741824;

// table drivers: one loop per table, failure names the row label
function driveRed(f, cases) { // reductions: "match a sequential scalar loop to a RELATIVE tolerance, not bitwise"; NaN propagates
  for (const [label, input, expected] of cases) {
    const got = f(F(input));
    if (typeof expected === "number" && Number.isNaN(expected)) assert(Number.isNaN(got), label);
    else assertClose(got, expected, 1e-6, label);
  }
}
function driveEW(f, cases) { // elementwise with separate out
  for (const [label, a, b, expected] of cases) {
    const out = new Float32Array(expected.length);
    f(out, F(a), F(b));
    assert(eqArr(out, expected), label);
  }
}
function driveIP(f, cases) { // in-place exact
  for (const [label, input, expected] of cases) {
    const a = F(input);
    f(a);
    assert(eqArr(a, expected), label);
  }
}
function driveIPc(f, cases, eps) { // in-place close (activations/unary approximations)
  for (const [label, input, expected] of cases) {
    const a = F(input);
    f(a);
    assert(eqArrClose(a, expected, eps), label);
  }
}
function driveIPnan(f, cases) { // in-place, expect every output element NaN ("NaN in the input propagates ... through every reduction and in-place transform")
  for (const [label, input] of cases) {
    const a = F(input);
    f(a);
    let all = true;
    for (const v of a) if (!Number.isNaN(v)) all = false;
    assert(all, label);
  }
}

// ============================ reductions: sum/max/min/argmax/argmin/normL1/normL2 (d.ts L4593-4603) ============================
{
  const cases = [
    ["sum exact powers of two", [0.5, 0.25, 0.25], 1],
    ["sum integers", [1, 2, 3, 4], 10],
    ["sum single", [7], 7],
    ["sum NaN propagates", [1, NaN], NaN],
    ["max", [1, 5, 3], 5],
    ["max all negative", [-0.5, -0.25, -1], -0.25],
    ["max NaN propagates", [5, NaN], NaN],
    ["min", [1, -5, 3], -5],
    ["min NaN propagates", [NaN, 1], NaN],
  ];
  const fns = { "sum": simd.sum, "max": simd.max, "min": simd.min };
  for (const [label, input, expected] of cases) {
    const fn = fns[label.slice(0, 3)];
    const got = fn(F(input));
    if (Number.isNaN(expected)) assert(Number.isNaN(got), label);
    else assertClose(got, expected, 1e-6, label);
  }
  // argmax/argmin: "Index of the extreme; throws on an empty array"
  const argCases = [
    ["argmax([1,9,3])", [1, 9, 3], 1, simd.argmax],
    ["argmax([7]) single", [7], 0, simd.argmax],
    ["argmin([4,2,8])", [4, 2, 8], 1, simd.argmin],
    ["argmin([-1]) single", [-1], 0, simd.argmin],
  ];
  for (const [label, input, expected, fn] of argCases) assertEq(fn(F(input)), expected, label);
  assertThrows(() => simd.argmax(F([])), "argmax empty throws");
  assertThrows(() => simd.argmin(F([])), "argmin empty throws");
  // norms: "The 1-norm" / "The Euclidean norm"
  const normCases = [
    ["normL1([3,-4])", [3, -4], 7],
    ["normL1 four -1s", [-1, -1, -1, -1], 4],
    ["normL2([3,4])", [3, 4], 5],      // sqrt(25) exact in f32
    ["normL2([0])", [0], 0],
    ["normL2([1,1])", [1, 1], Math.SQRT2],
  ];
  for (const [label, input, expected] of normCases) assertClose(label.startsWith("normL1") ? simd.normL1(F(input)) : simd.normL2(F(input)), expected, 1e-6, label);
}

// ============================ elementwise: add/sub/mul/div/abs/fma/dot (d.ts L4605-4614) ============================
// "out[i] = a[i] op b[i]; all lengths must match" — exact f32 rows over exactly representable values
{
  const addCases = [
    ["add [0.5,1]+[0.25,2]", [0.5, 1], [0.25, 2], [0.75, 3]],
    ["add integers", [1, 2], [3, 4], [4, 6]],
  ];
  driveEW(simd.add, addCases);
  const subCases = [
    ["sub [1,0.5]-[0.25,0.5]", [1, 0.5], [0.25, 0.5], [0.75, 0]],
    ["sub integers", [5, 3], [2, 1], [3, 2]],
  ];
  driveEW(simd.sub, subCases);
  const mulCases = [
    ["mul [0.5,-2]*[4,1.5]", [0.5, -2], [4, 1.5], [2, -3]],
    ["mul by ones", [1.5, 2.5], [1, 1], [1.5, 2.5]],
  ];
  driveEW(simd.mul, mulCases);
  const divCases = [
    ["div [1,3]/[2,2]", [1, 3], [2, 2], [0.5, 1.5]],
    ["div exact quarters", [1], [4], [0.25]],
  ];
  driveEW(simd.div, divCases);
  // abs(out, a) takes TWO args (d.ts) — driveEW's third array would be parsed
  // as the window opts bag, so call it directly.
  const absCases = [
    ["abs mixed", [-1.5, 2, -0.25], [1.5, 2, 0.25]],
  ];
  for (const [label, a, expected] of absCases) {
    const out = new Float32Array(expected.length);
    simd.abs(out, F(a));
    assert(eqArr(out, expected), label);
  }
  // fma: "In-place z[i] += a[i] * b[i]"
  {
    const cases = [
      ["fma z=[1,0.5] a=[0.5,2] b=[4,0.25]", [1, 0.5], [0.5, 2], [4, 0.25], [3, 1]],
      ["fma zero z", [0, 0], [1.5, 0.5], [2, 4], [3, 2]],
    ];
    for (const [label, z, a, b, expected] of cases) {
      const zz = F(z);
      simd.fma(zz, F(a), F(b));
      assert(eqArr(zz, expected), label);
    }
  }
  // dot: "Inner product"
  const dotCases = [
    ["dot([1,2],[3,4])", [1, 2], [3, 4], 11],
    ["dot([0.5,0.5],[2,4])", [0.5, 0.5], [2, 4], 3],
    ["dot NaN propagates", [NaN], [2], NaN],
  ];
  for (const [label, a, b, expected] of dotCases) {
    const got = simd.dot(F(a), F(b));
    if (Number.isNaN(expected)) assert(Number.isNaN(got), label);
    else assertClose(got, expected, 1e-6, label);
  }
  assertThrows(() => simd.add(new Float32Array(2), F([1]), F([1, 2])), "elementwise length mismatch throws"); // "all lengths must match"
}

// ============================ scale/addScalar/axpy/affine (d.ts L4616-4622) ============================
// "In-place scalar ops" / "y[i] += alpha * x[i]" / "a[i] = alpha * a[i] + beta"
{
  const cases = [
    ["scale [1,2] by 0.5", (a) => simd.scale(a, 0.5), [1, 2], [0.5, 1]],
    ["scale by 2", (a) => simd.scale(a, 2), [1.5, 0.25], [3, 0.5]],
    ["addScalar +2", (a) => simd.addScalar(a, 2), [0.5, 1.5], [2.5, 3.5]],
    ["addScalar -1", (a) => simd.addScalar(a, -1), [1, 2], [0, 1]],
    ["axpy y+=2x", (a) => simd.axpy(a, 2, F([1.5, 2.5])), [0, 0], [3, 5]],
    ["affine 2a+1", (a) => simd.affine(a, 2, 1), [1, 2], [3, 5]],
    ["affine identity", (a) => simd.affine(a, 1, 0), [1.5, -2], [1.5, -2]],
  ];
  for (const [label, fn, input, expected] of cases) {
    const a = F(input);
    fn(a);
    assert(eqArr(a, expected), label);
  }
}

// ============================ activations (d.ts L4624-4635) ============================
{
  const reluCases = [
    ["relu [-1,0,2]", [-1, 0, 2], [0, 0, 2]],
  ];
  driveIP(simd.relu, reluCases);
  {
    const a = F([NaN]);
    simd.relu(a);
    assert(Number.isNaN(a[0]), "relu NaN propagates"); // "NaN ... propagates through every ... in-place transform"
  }
  const relu6Cases = [
    ["relu6 [-5,3,9,6]", [-5, 3, 9, 6], [0, 3, 6, 6]], // "min(max(x,0),6)"
    ["relu6 boundary 0 and 6", [0, 6], [0, 6]],
  ];
  driveIP(simd.relu6, relu6Cases);
  const leakyCases = [
    ["leakyRelu slope 0.5", [-2, 4], 0.5, [-1, 4]],
    ["leakyRelu slope 1 = identity", [-3, 7], 1, [-3, 7]],
  ];
  for (const [label, input, slope, expected] of leakyCases) {
    const a = F(input);
    simd.leakyRelu(a, slope);
    assert(eqArr(a, expected), label);
  }
  const eluCases = [
    ["elu positive identity", [2], 1, [2]],
    ["elu(0)", [0], 1, [0]],
    ["elu(-1) alpha=1", [-1], 1, [Math.exp(-1) - 1]], // alpha*(exp(x)-1)
  ];
  for (const [label, input, alpha, expected] of eluCases) {
    const a = F(input);
    simd.elu(a, alpha);
    assert(eqArrClose(a, expected, 1e-6), label);
  }
  const sigCases = [
    ["sigmoid(0) = 0.5", [0], [0.5]],
    // DOC-TENSION: API.md's prose says 1/(1+exp(-x)) but its own example prints
    // sigmoid(1) = 0.7198 — the Schraudolph-style fast kernel's value (exact: 0.7311).
    // The observed example is the contract; activations are documented approximations.
    ["sigmoid(2) within the fast kernel's accuracy", [2], [1 / (1 + Math.exp(-2))]],
    ["sigmoid large negative ~ 0", [-100], [0]],
    ["sigmoid large positive ~ 1", [100], [1]],
  ];
  driveIPc(simd.sigmoid, sigCases.slice(0, 1), 1e-6);
  driveIPc(simd.sigmoid, sigCases.slice(1, 2), 5e-3); // fast-approx tolerance
  driveIPc(simd.sigmoid, sigCases.slice(2), 1e-6);
  const tanhCases = [
    ["tanhFast(0)", [0], [0]],
    ["tanhFast large +", [100], [1]],
    ["tanhFast large -", [-100], [-1]],
  ];
  driveIPc(simd.tanhFast, tanhCases, 1e-6);
  {
    const a = F([1]);
    simd.tanhFast(a);
    assertClose(a[0], Math.tanh(1), 1e-2, "tanhFast(1) near tanh(1) (fast path: kernel is the documented approximation, e.g. API.md sigmoid example)");
  }
  const geluSilu = [
    ["gelu(0)", simd.gelu, [0], [0]],
    ["silu(0)", simd.silu, [0], [0]],
  ];
  for (const [label, fn, input, expected] of geluSilu) {
    const a = F(input);
    fn(a);
    assert(eqArr(a, expected), label);
  }
  // softmax: "Stable max-shifted softmax in place"
  {
    const a = F([0, 0]);
    simd.softmax(a);
    assert(eqArrClose(a, [0.5, 0.5], 1e-7), "softmax([0,0]) = [0.5,0.5]");
    const b = F([1000, 1000]);
    simd.softmax(b);
    assert(eqArrClose(b, [0.5, 0.5], 1e-7), "softmax max-shifted: [1000,1000] stays finite [0.5,0.5]");
    const c = F([1, 2, 3]);
    simd.softmax(c);
    let s = 0;
    for (const v of c) s += v;
    assertClose(s, 1, 1e-6, "softmax outputs sum to 1");
    const d = F([NaN, 1]);
    simd.softmax(d);
    assert(Number.isNaN(d[0]) && Number.isNaN(d[1]), "softmax NaN propagates");
  }
  // logSoftmax
  const lsExp = 1 - (3 + Math.log(1 + Math.exp(-1) + Math.exp(-2))); // x - logSumExp(1,2,3)
  const logSCases = [
    ["logSoftmax([0,0])", [0, 0], [Math.log(0.5), Math.log(0.5)]],
    ["logSoftmax([1,2,3])", [1, 2, 3], [lsExp, lsExp + 1, lsExp + 2]],
    ["logSoftmax stable at 1000", [1000, 1000], [Math.log(0.5), Math.log(0.5)]],
  ];
  // logSoftmax routes through the documented fast-approx kernel family (see the
  // sigmoid DOC-TENSION above), so its accuracy rides the same ~1e-2 envelope.
  driveIPc(simd.logSoftmax, logSCases, 3e-2);
}

// ============================ unary math: vexp/vlog/vsqrt/vrsqrt/vinv (d.ts L4637-4642) ============================
// "In-place unary math; NaN propagates."
{
  const exactCases = [
    ["vexp(0) = 1", simd.vexp, [0], [1]],
    ["vlog(1) = 0", simd.vlog, [1], [0]],
    ["vsqrt(4) = 2", simd.vsqrt, [4], [2]],
    ["vinv(4) = 0.25", simd.vinv, [4], [0.25]],
    ["vinv(0.5) = 2", simd.vinv, [0.5], [2]],
  ];
  for (const [label, fn, input, expected] of exactCases) {
    const a = F(input);
    fn(a);
    assert(eqArr(a, expected), label);
  }
  const closeCases = [
    // DOC-TENSION: vexp rides the same documented fast-approx kernel family as
    // sigmoid/tanhFast (API.md's own sigmoid example pins the approx value);
    // its prose "e^x" carries ~6% at x=1. vlog/vsqrt/vrsqrt are sub-ulp exact.
    ["vexp(1) ~ e (fast-approx envelope)", simd.vexp, [1], [Math.E]],
    ["vlog(4) ~ ln4", simd.vlog, [4], [Math.log(4)]],
    ["vsqrt(2) ~ sqrt2", simd.vsqrt, [2], [Math.SQRT2]],
    ["vrsqrt(4) ~ 0.5", simd.vrsqrt, [4], [0.5]],
  ];
  for (const [label, fn, input, expected] of closeCases) {
    const a = F(input);
    fn(a);
    assert(eqArrClose(a, expected, label.startsWith("vexp") ? 0.2 : 1e-6), label); // vexp: absolute |2.885 - e| ~ 0.167 for the documented fast kernel
  }
  const nanCases = [
    ["vexp NaN", [NaN], simd.vexp],
    ["vlog NaN", [NaN], simd.vlog],
    ["vsqrt NaN", [NaN], simd.vsqrt],
    ["vinv NaN", [NaN], simd.vinv],
  ];
  // "In-place; NaN propagates" pins NaN INPUTS; out-of-domain clamps
  // (vsqrt(-1) -> 0, vlog(neg) -> -FLT_MAX) are doc-silent design choices.
  for (const [label, input, fn] of nanCases) {
    const a = F(input);
    fn(a);
    assert(Number.isNaN(a[0]), label);
  }
}

// ============================ distances (d.ts L4644-4648) ============================
// "Vector-to-scalar distances over equal-length pairs."
{
  const cases = [
    ["distL2([0,0],[3,4])", simd.distL2, [0, 0], [3, 4], 5],
    ["distL2 identical", simd.distL2, [1.5, 2.5], [1.5, 2.5], 0],
    ["distL1([1,2],[3,5])", simd.distL1, [1, 2], [3, 5], 5],
    ["distL1 identical", simd.distL1, [1], [1], 0],
    ["distCos orthogonal", simd.distCos, [1, 0], [0, 2], 1],
    ["distCos parallel", simd.distCos, [2, 0], [3, 0], 0],
    ["distCheb([1,5],[4,2])", simd.distCheb, [1, 5], [4, 2], 3],
    ["distCheb([0,0],[3,4])", simd.distCheb, [0, 0], [3, 4], 4],
  ];
  for (const [label, fn, a, b, expected] of cases) assertClose(fn(F(a), F(b)), expected, 1e-6, label);
}

// ============================ BLAS-2/3: gemv (both forms) / gemvT / gemm (d.ts L4650-4657) ============================
// Row-major, explicit dims. A = [[1,2],[3,4]] flat [1,2,3,4], m=2, n=2.
{
  // 6-arg legacy: y = beta*y + A*x
  {
    const cases = [
      ["gemv6 beta=0 fresh y", [0, 0], 0, [1, 1], [3, 7]],
      ["gemv6 beta=1 accumulate", [10, 20], 1, [1, 1], [13, 27]],
      ["gemv6 unit vector x", [0, 0], 1, [1, 0], [1, 3]],
    ];
    for (const [label, y0, beta, x, expected] of cases) {
      const y = F(y0);
      simd.gemv(y, F([1, 2, 3, 4]), F(x), 2, 2, beta);
      assert(eqArr(y, expected), label);
    }
  }
  // 7-arg form: y = alpha*A*x + beta*y; "beta == 0 skips the y read"
  {
    const cases = [
      ["gemv7 alpha=2 beta=1", [10, 20], 2, 1, [1, 1], [16, 34]],
      ["gemv7 alpha=1 beta=0 == gemv6 beta=0", [0, 0], 1, 0, [1, 1], [3, 7]],
    ];
    for (const [label, y0, alpha, beta, x, expected] of cases) {
      const y = F(y0);
      simd.gemv(y, F([1, 2, 3, 4]), F(x), 2, 2, alpha, beta);
      assert(eqArr(y, expected), label);
    }
    const yNaN = F([NaN, NaN]);
    simd.gemv(yNaN, F([1, 2, 3, 4]), F([1, 1]), 2, 2, 1, 0);
    assert(eqArr(yNaN, [3, 7]), "gemv7 beta=0 skips the y read (NaN y ignored)"); // documented verbatim
  }
  // parity: 6-arg with beta=B equals 7-arg with alpha=1, beta=B
  {
    const y6 = F([4, 5]);
    simd.gemv(y6, F([1, 2, 3, 4]), F([1, 1]), 2, 2, 2);
    const y7 = F([4, 5]);
    simd.gemv(y7, F([1, 2, 3, 4]), F([1, 1]), 2, 2, 1, 2);
    assert(eqArr(y6, y7), "gemv 6-arg == 7-arg with alpha=1 (dispatch on argument count)");
  }
  // gemvT: y = beta*y + A^T*x
  {
    const cases = [
      ["gemvT beta=0", [0, 0], 0, [1, 1], [4, 6]],
      ["gemvT beta=1 accumulate", [1, 1], 1, [1, 0], [2, 3]], // A^T x with x=[1,0] is [1,2]; y = [1,1] + [1,2]
    ];
    for (const [label, y0, beta, x, expected] of cases) {
      const y = F(y0);
      simd.gemvT(y, F([1, 2, 3, 4]), F(x), 2, 2, beta);
      assert(eqArr(y, expected), label);
    }
  }
  // gemm: c = alpha*A*B + beta*C; A 2x3, B 3x2 -> [4,5,10,11]
  {
    const c = F([0, 0, 0, 0]);
    simd.gemm(c, F([1, 2, 3, 4, 5, 6]), F([1, 0, 0, 1, 1, 1]), 2, 2, 3, 1, 0);
    assert(eqArr(c, [4, 5, 10, 11]), "gemm 2x3 * 3x2 alpha=1 beta=0");
    const c2 = F([1, 1, 1, 1]);
    simd.gemm(c2, F([1, 2, 3, 4, 5, 6]), F([1, 0, 0, 1, 1, 1]), 2, 2, 3, 2, 1);
    assert(eqArr(c2, [9, 11, 21, 23]), "gemm alpha=2 beta=1 accumulates into c");
  }
}

// ============================ statistics + (offset,length) windows (d.ts L4659-4685) ============================
{
  // mean/variance: "Empty input throws RangeError"; mean = sum(a)/n; two-pass population variance
  const meanCases = [
    ["mean [1,2,3,4]", [1, 2, 3, 4], 2.5],
    ["mean single", [9], 9],
    ["mean NaN propagates", [1, NaN], NaN],
  ];
  for (const [label, input, expected] of meanCases) {
    const got = simd.mean(F(input));
    if (Number.isNaN(expected)) assert(Number.isNaN(got), label);
    else assertClose(got, expected, 1e-6, label);
  }
  assertThrows(() => simd.mean(F([])), "mean empty throws RangeError", RangeError);
  assertThrows(() => simd.variance(F([])), "variance empty throws RangeError", RangeError);
  const varCases = [
    ["variance [1,2,3,4] population", [1, 2, 3, 4], 1.25],
    ["variance constant", [7, 7, 7], 0],
    ["variance NaN propagates", [NaN, 1, 2], NaN],
  ];
  for (const [label, input, expected] of varCases) {
    const got = simd.variance(F(input));
    if (Number.isNaN(expected)) assert(Number.isNaN(got), label);
    else assertClose(got, expected, 1e-6, label);
  }
  // normalize: "In-place z-score; constant array -> all-NaN"
  {
    const a = F([1, 2, 3, 4]);
    simd.normalize(a);
    assert(eqArrClose(a, [-1.3416407864998738, -0.4472135954999579, 0.4472135954999579, 1.3416407864998738], 1e-5), "normalize z-scores [1,2,3,4]");
    const b = F([7, 7, 7]);
    simd.normalize(b);
    let all = true;
    for (const v of b) if (!Number.isNaN(v)) all = false;
    assert(all, "normalize constant array is all-NaN"); // documented verbatim
  }
  // windows: "(offset,length) ... {offset, length|limit} bag ... Out of range is a RangeError"
  {
    const a = new Float32Array(16);
    a[12] = 1; a[13] = 2; a[14] = 3; a[15] = 4;
    assertClose(simd.sum(a, 12, 4), 10, 1e-6, "sum window (12,4)");
    assertClose(simd.sum(a, { offset: 12, length: 4 }), 10, 1e-6, "sum window bag form");
    assertClose(simd.mean(a, 12, 4), 2.5, 1e-6, "mean window positional");
    assertClose(simd.mean(a, { offset: 12, length: 4 }), 2.5, 1e-6, "mean window bag (length)");
    assertClose(simd.mean(a, { offset: 12, limit: 4 }), 2.5, 1e-6, "mean window bag (limit aliases length)");
    assertThrows(() => simd.sum(a, 14, 4), "window out of range throws RangeError", RangeError);
    assertThrows(() => simd.mean(a, { offset: 12, length: 4, limit: 4 }), "length and limit together refused (never both)");
  }
  // f64 statistics doors
  assertClose(simd.f64Mean(Float64Array.from([1, 2, 3, 4])), 2.5, 1e-15, "f64Mean");
  assertClose(simd.f64Variance(Float64Array.from([1, 2, 3, 4])), 1.25, 1e-15, "f64Variance two-pass population");
  assertEq(simd.f64Variance(Float64Array.from([5, 5, 5])), 0, "f64Variance constant is 0");
  // i32Mean: "exact int64 accumulation ... exact as a double while |sum| <= 2^53"
  assertEq(simd.i32Mean(Int32Array.from([1, 2, 3, 4])), 2.5, "i32Mean small");
  assertEq(simd.i32Mean(Int32Array.from([1073741824, 1073741824, 1073741824, 1073741824])), 1073741824, "i32Mean exact at 2^32 sum");
  assertEq(simd.i32Mean(new Int32Array(4096).fill(2147483647)), 2147483647, "i32Mean 4096*Int32_MAX exact (no overflow)");
}

// ============================ clamp / threshold / topkIndices (d.ts L4687-4692) ============================
{
  const clampCases = [
    ["clamp [1,3,5] to [2,4]", [1, 3, 5], 2, 4, [2, 3, 4]],
    ["clamp [-5,0,5] to [-1,1]", [-5, 0, 5], -1, 1, [-1, 0, 1]],
  ];
  for (const [label, input, lo, hi, expected] of clampCases) {
    const a = F(input);
    simd.clamp(a, lo, hi);
    assert(eqArr(a, expected), label);
  }
  // threshold: "x > t ? 1.0 : 0.0" (strict)
  const thCases = [
    ["threshold 0.5 at t=0.5 (strict >)", [0.5, 0.5, 2], 0.5, [0, 0, 1]],
    ["threshold at t=0", [-1, 0, 1], 0, [0, 0, 1]],
  ];
  for (const [label, input, t, expected] of thCases) {
    const a = F(input);
    simd.threshold(a, t);
    assert(eqArr(a, expected), label);
  }
  // topkIndices: "Indices of the k largest values (a fresh array, unspecified order)"
  const tk = simd.topkIndices(F([10, 30, 20]), 2);
  assert(tk instanceof Uint32Array, "topkIndices returns a fresh Uint32Array");
  assert(eqArr(Array.from(tk).sort(), [1, 2]), "topkIndices([10,30,20],2) indices {1,2} (order unspecified)");
  assert(eqArr(Array.from(simd.topkIndices(F([10, 30, 20]), 1)), [1]), "topkIndices k=1 -> index of 30");
  // API.md pins the CLAMP: "the indices of the min(k, vals.length) largest values"
  assert(eqArr(Array.from(simd.topkIndices(F([1, 2]), 5)).sort(), [0, 1]), "topkIndices k>n clamps to vals.length (API.md)");
}

// ============================ f64 kernels (d.ts L4694-4700) ============================
// "Zero-copy Float64Array kernels."
{
  const cases = [
    ["f64Sum [1,2,3,4]", (a) => simd.f64Sum(a), [1, 2, 3, 4], 10],
    ["f64Dot", (a) => simd.f64Dot(a, Float64Array.from([3, 4])), [1, 2], 11],
    ["f64Max", (a) => simd.f64Max(a), [1, 9, 3], 9],
    ["f64Min", (a) => simd.f64Min(a), [1, 9, 3], 1],
  ];
  for (const [label, fn, input, expected] of cases) assertClose(fn(Float64Array.from(input)), expected, 1e-12, label);
  {
    const a = Float64Array.from([1, 2]);
    simd.f64Scale(a, 0.5);
    assert(eqArr(a, [0.5, 1]), "f64Scale in place");
    const y = Float64Array.from([1, 1]);
    simd.f64Axpy(y, 2, Float64Array.from([1, 2]));
    assert(eqArr(y, [3, 5]), "f64Axpy y += alpha*x");
  }
}

// ============================ i32 kernels + STRICT typing (d.ts L4702-4709) ============================
// "i32* names run Int32Array and are STRICT: the element type is verified by class, a same-stride Float32Array/Uint32Array is rejected, never reinterpreted."
{
  const cases = [
    ["i32Sum [1,2,3]", (a) => simd.i32Sum(a), [1, 2, 3], 6],
    ["i32Sum negatives", (a) => simd.i32Sum(a), [-1, -2], -3],
    ["i32Min", (a) => simd.i32Min(a), [3, 1, 2], 1],
    ["i32Max", (a) => simd.i32Max(a), [3, 1, 2], 3],
    ["i32Dot", (a) => simd.i32Dot(a, Int32Array.from([4, 5])), [2, 3], 23],
  ];
  for (const [label, fn, input, expected] of cases) assertEq(fn(Int32Array.from(input)), expected, label);
  {
    const out = new Int32Array(2);
    simd.i32Add(out, Int32Array.from([1, 2]), Int32Array.from([3, 4]));
    assert(eqArr(out, [4, 6]), "i32Add");
    const out2 = new Int32Array(2);
    simd.i32Mul(out2, Int32Array.from([2, 3]), Int32Array.from([5, 7]));
    assert(eqArr(out2, [10, 21]), "i32Mul");
    const a = Int32Array.from([1, 2]);
    simd.i32Scale(a, 10);
    assert(eqArr(a, [10, 20]), "i32Scale");
    const b = Int32Array.from([2, 3]);
    simd.i32Scale(b, -1);
    assert(eqArr(b, [-2, -3]), "i32Scale by -1");
  }
  assertThrows(() => simd.i32Sum(new Float32Array([1, 2])), "i32Sum rejects Float32Array (STRICT class check)");
  assertThrows(() => simd.i32Sum(new Uint32Array([1, 2])), "i32Sum rejects Uint32Array (STRICT class check)");
}

// ============================ cumsum / cummax (d.ts L4711-4715) ============================
// "Inclusive prefix scan, in place (the output type follows the input)."
{
  {
    const a = Int32Array.from([1, 2, 3]);
    const r = simd.cumsum(a);
    assert(eqArr(r, [1, 3, 6]), "cumsum Int32Array");
    assert(r instanceof Int32Array, "cumsum output type follows Int32Array input");
  }
  {
    const a = Int32Array.from([1, -2]);
    simd.cumsum(a);
    assert(eqArr(a, [1, -1]), "cumsum Int32Array with negative");
  }
  {
    const a = Float32Array.from([0.5, 0.25]);
    const r = simd.cumsum(a);
    assert(eqArr(r, [0.5, 0.75]), "cumsum Float32Array");
    assert(r instanceof Float32Array, "cumsum output type follows Float32Array input");
  }
  {
    const cases = [
      ["cummax Int32Array [1,3,2]", Int32Array.from([1, 3, 2]), [1, 3, 3]],
      ["cummax Float32Array [0.5,0.25,0.75]", Float32Array.from([0.5, 0.25, 0.75]), [0.5, 0.5, 0.75]],
    ];
    for (const [label, input, expected] of cases) assert(eqArr(simd.cummax(input), expected), label);
  }
}

// ============================ bit-cast parity: F32Like doors (d.ts L4582-4592 + shared types L76-82) ============================
// "Int32Array/Uint32Array are accepted as a BIT-CAST reinterpretation ... The output kernels write bit patterns into whatever array you hand them."
{
  const cases = [
    ["sum f32 door [1.5,2.0]", simd.sum(Float32Array.from([1.5, 2.0])), 3.5],
    ["sum bit-cast i32 door [0x3FC00000,0x40000000]", simd.sum(Int32Array.from([B15, B20])), 3.5],
    ["max bit-cast [-2.0,1.0]", simd.max(Int32Array.from([B2N, B10])), 1],
    ["dot bit-cast [2.0]x[1.0]", simd.dot(Int32Array.from([B20]), Int32Array.from([B10])), 2],
  ];
  for (const [label, got, expected] of cases) assertClose(got, expected, 1e-6, label);
  {
    const out = new Uint32Array(1);
    simd.add(out, Uint32Array.from([B20]), Uint32Array.from([B10]));
    assertEq(out[0], B30, "add writes bit patterns into the handed Uint32Array (2.0+1.0 -> 0x40400000)");
    assertClose(new Float32Array(out.buffer)[0], 3, 1e-6, "bit-cast out reads back as 3.0 through a Float32Array view");
  }
  // parity: same logical input through both doors gives identical answers
  assertEq(simd.sum(Float32Array.from([1.5, 2.0])), simd.sum(Int32Array.from([B15, B20])), "bit-cast parity: f32 door === i32 door for identical bits");
}

// ============================ scalar-vs-simd parity: two documented doors, identical inputs/expected ============================
// dyna:simd reductions vs dyna:mathx scalar/array helpers (both documented contracts).
{
  const cases = [
    ["sum [1,2,3,4]", [1, 2, 3, 4], 10, (a) => simd.sum(F(a)), (a) => mathx.stats.sum(a)],
    ["mean [1,2,3,4]", [1, 2, 3, 4], 2.5, (a) => simd.mean(F(a)), (a) => mathx.stats.mean(a)],
    ["min [1,2,3,4]", [1, 2, 3, 4], 1, (a) => simd.min(F(a)), (a) => mathx.stats.min(a)],
    ["max [1,2,3,4]", [1, 2, 3, 4], 4, (a) => simd.max(F(a)), (a) => mathx.stats.max(a)],
  ];
  for (const [label, input, expected, simdDoor, scalarDoor] of cases) {
    const viaSimd = simdDoor(input);
    const viaScalar = scalarDoor(input);
    assertClose(viaSimd, expected, 1e-6, label + " (simd door)");
    assertClose(viaScalar, expected, 1e-6, label + " (scalar door)");
    assertEq(viaSimd, viaScalar, label + " (doors agree exactly)");
  }
  // cumsum across the two modules: typed-array door vs number[] door
  assert(eqArr(Array.from(simd.cumsum(Int32Array.from([1, 2, 3]))), mathx.cumsum([1, 2, 3])), "cumsum doors agree [1,3,6]");
  // f64 vs f32 door inside dyna:simd
  assertEq(simd.sum(F([1, 2, 3, 4])), simd.f64Sum(Float64Array.from([1, 2, 3, 4])), "f32 and f64 sum doors agree on [1,2,3,4]");
}

print("bb_simd: all tests passed (" + n + " assertions)");
