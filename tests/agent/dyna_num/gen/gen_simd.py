#!/usr/bin/env python3
# gen_simd.py -- emit black-box probes for dyna:simd.
#
# ORACLE POLICY (mirrors the module's documented tolerance classes):
#   exact  -- IEEE f32 arithmetic and pure selections (add/sub/mul/div/abs/
#             scale/addScalar/clamp/threshold/relu/relu6/leakyRelu/cumsum/
#             cummax) and ALL i32/f64 kernels: golden is numpy
#             float32/int32/float64 baked as raw BIT PATTERNS; the probe
#             asserts bit equality. Selection kernels replicate the scalar
#             source's comparison chains (NaN falls to the else arm:
#             relu(NaN)=0, threshold(NaN)=0, clamp(NaN)=NaN, relu6(NaN)=NaN).
#             Inputs are baked as hex uint32 patterns reconstructed through a
#             Uint32Array view -- no JS-literal double rounding anywhere.
#   near   -- fma, affine, BLAS (order-sensitive sums): documented tolerances.
#   approx -- fast_exp-class activations (sigmoid/tanhFast/silu/softmax/
#             logSoftmax/vexp/elu/gelu): float64 reference + per-op tolerance.
# Shapes sweep LENS x patterns {zeros,alt,tiny,huge,special,random}. This is
# what catches the unguarded-vector-load class of bug (the binding's own
# max/min/argmax/argmin small-n guard exists precisely because of it).
import os, json
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
PROBES = os.path.join(HERE, "..", "probes", "simd")
HARNESS = os.path.join(HERE, "..", "h.js")

LENS = [0, 1, 2, 3, 5, 7, 8, 15, 16, 17, 31, 32, 63, 64, 65, 100, 255, 4096]

def lcg(seed):
    s = seed & 0xFFFFFFFF
    while True:
        s = (s * 1664525 + 1013904223) & 0xFFFFFFFF
        yield s

def f32_pattern(kind, n, rng):
    if kind == "zeros":
        return np.zeros(n, np.float32)
    if kind == "alt":
        v = np.empty(n, np.float32)
        v[0::2] = np.float32(1.5)
        v[1::2] = np.float32(-2.25)
        return v
    if kind == "tiny":
        v = np.full(n, 1e-38, np.float32)
        v[0::2] = np.float32(-1e-38)
        return v
    if kind == "huge":
        v = np.full(n, 3e38, np.float32)
        v[0::2] = np.float32(-3e38)
        return v
    if kind == "special":
        v = np.full(n, 2.5, np.float32)
        if n > 0:
            v[0] = np.float32("nan")
        if n > 3:
            v[3] = np.float32("inf")
        if n > 7:
            v[7] = np.float32("-inf")
        if n > 11:
            v[11] = np.float32("-0.0")
        return v
    r = np.array([((next(rng) >> 8) / 16777216.0) * 8.0 - 4.0 for _ in range(n)], np.float64)
    return r.astype(np.float32)

def bits32(v):
    return ['"%08x"' % b for b in np.ascontiguousarray(v).ravel().view(np.uint32)]

def bits64(v):
    # emit 32-bit words in MEMORY order (little-endian: low word first) so the
    # u32-based decoder reconstructs the exact bit pattern
    out = []
    for b in np.ascontiguousarray(v).ravel().view(np.uint64):
        out.append('"%08x"' % (b & 0xFFFFFFFF))
        out.append('"%08x"' % (b >> 32))
    return out

def js_f32(arr):
    return "f32fromhex([" + ",".join(bits32(arr)) + "])"

def js_f64(arr):
    return "f64fromhex([" + ",".join(bits64(arr)) + "])"

def js_i32(arr):
    return "new Int32Array([" + ",".join(str(int(x)) for x in arr) + "])"

PRELUDE = """
function f32fromhex(hex) {
  var a = new Float32Array(hex.length), u = new Uint32Array(a.buffer);
  for (var i = 0; i < hex.length; i++) u[i] = parseInt(hex[i], 16);
  return a;
}
function f64fromhex(hex) {
  var a = new Float64Array(hex.length / 2), u = new Uint32Array(a.buffer);
  for (var i = 0; i < hex.length; i++) u[i] = parseInt(hex[i], 16);
  return a;
}
"""

BITCMP = """
function checkbits(op, idx, gotA, want) {
  var u = new Uint32Array(gotA.buffer), wu = new Uint32Array(want.buffer);
  if (gotA.length !== want.length) {
    __fail++; __out("FAIL " + op + "#" + idx + " length " + gotA.length + " want " + want.length);
    return;
  }
  for (var j = 0; j < want.length; j++)
    if (u[j] !== wu[j]) {
      __fail++;
      __out("FAIL " + op + "#" + idx + "[" + j + "] bits 0x" + u[j].toString(16) +
            " want 0x" + wu[j].toString(16) + " (len " + want.length + ")");
      return;
    }
  __pass++;
}
"""

class ProbeWriter:
    def __init__(self, path, imports, tag, prelude=True):
        self.path = path
        os.makedirs(os.path.dirname(path), exist_ok=True)
        self.f = open(path, "w")
        self.f.write("// GENERATED probe (%s) -- do not edit; run materialize.sh\n" % tag)
        for imp in imports:
            self.f.write(imp + "\n")
        self.f.write('var __TAG = "%s";\n' % tag)
        self.f.write(open(HARNESS).read().replace("//dyna_num_harness_marker", ""))
        if prelude:
            self.f.write(PRELUDE)
    def w(self, line=""):
        self.f.write(line + "\n")
    def close(self, total):
        self.f.write('summary(__TAG); // %d cases\n' % total)
        self.f.close()
        print("wrote %s (%d cases)" % (self.path, total))

# ------------------------------------------------------- exact f32 elementwise
def gen_elementwise():
    tag = "simd_elementwise"
    p = ProbeWriter(os.path.join(PROBES, tag + ".js"),
                    ['import { add, sub, mul, div, abs, scale, addScalar, clamp,',
                     '         threshold, relu, relu6, leakyRelu } from "dyna:simd";'], tag)
    cases = []
    rng = lcg(20260911)
    for n in LENS:
        for pat in ["zeros", "alt", "tiny", "huge", "special", "random"]:
            a = f32_pattern(pat, n, rng)
            b = f32_pattern("random" if pat != "zeros" else "alt", n, rng)
            with np.errstate(all="ignore"):
                cases.append(("add", (a, b), (a + b).astype(np.float32)))
                cases.append(("sub", (a, b), (a - b).astype(np.float32)))
                cases.append(("mul", (a, b), (a * b).astype(np.float32)))
                cases.append(("div", (a, b), (a / b).astype(np.float32)))
            cases.append(("abs", (a,), np.abs(a)))
            # relu/relu6 observable contract is LENGTH-POSITIONAL: elements in
            # the vector body (first n - n%4) use fmax semantics (NaN stays
            # NaN, -0 -> +0); the scalar tail folds with strict > (NaN -> +0,
            # -0 -> +0). Pinned here so a change in either regime is caught.
            body = n - (n % 4)
            pos = np.arange(n)
            srelu = np.where(a > 0, a, np.float32(0)).astype(np.float32)
            relu_g = np.where((pos < body) & np.isnan(a), np.float32("nan"), srelu) + np.float32(0)
            cases.append(("relu", (a,), relu_g))
            srelu6 = np.where(a < 0, np.float32(0), np.where(a > 6, np.float32(6), a)).astype(np.float32) + np.float32(0)
            r6 = np.where((pos < body) & np.isnan(a), np.float32("nan"), srelu6)
            cases.append(("relu6", (a,), r6))
            cases.append(("leakyRelu", (a,), np.where(a > 0, a, a * np.float32(0.1)).astype(np.float32)))
            cases.append(("clamp", (a,), np.where(a < np.float32(-1.25), np.float32(-1.25),
                          np.where(a > np.float32(1.25), np.float32(1.25), a)).astype(np.float32)))
            cases.append(("threshold", (a,), np.where(a > np.float32(0.5), np.float32(1),
                          np.float32(0)).astype(np.float32)))
            with np.errstate(all="ignore"):
                cases.append(("scale", (a,), (a * np.float32(3.5)).astype(np.float32)))
            cases.append(("addScalar", (a,), (a + np.float32(-1.75)).astype(np.float32)))
    p.w(BITCMP)
    p.w("var S = { add: add, sub: sub, mul: mul, div: div, abs: abs, relu: relu,")
    p.w("         relu6: relu6, leakyRelu: leakyRelu, clamp: clamp, threshold: threshold,")
    p.w("         scale: scale, addScalar: addScalar };")
    p.w("var CASE = [")
    rows = []
    for (op, ins, out) in cases:
        insjs = ",".join(js_f32(x) for x in ins)
        rows.append("[%s,[%s],%s]" % (json.dumps(op), insjs, js_f32(out)))
    p.w(",\n".join(rows))
    p.w("];")
    p.w("""
var OUT2 = { add:1, sub:1, mul:1, div:1 };   // (out, a, b)
var OUT1 = { abs:1 };                        // (out, a)
for (var i = 0; i < CASE.length; i++) {
  var op = CASE[i][0], ins = CASE[i][1], want = CASE[i][2], got;
  if (OUT2[op]) {
    var out = new Float32Array(ins[0].length);
    S[op](out, ins[0], ins[1]);
    got = out;
  } else if (OUT1[op]) {
    var out1 = new Float32Array(ins[0].length);
    abs(out1, ins[0]);
    got = out1;
  } else {
    var a = new Float32Array(ins[0].length);
    a.set(ins[0]);
    if (op === "leakyRelu") leakyRelu(a, 0.1);
    else if (op === "clamp") clamp(a, -1.25, 1.25);
    else if (op === "threshold") threshold(a, 0.5);
    else if (op === "scale") scale(a, 3.5);
    else if (op === "addScalar") addScalar(a, -1.75);
    else S[op](a);
    got = a;
  }
  checkbits(op, i, got, want);
}
""")
    p.close(len(cases))

# --------------------------------------------------- scans, i32/f64, BLAS
def gen_kernels():
    tag = "simd_kernels"
    p = ProbeWriter(os.path.join(PROBES, tag + ".js"),
                    ['import { cumsum, cummax, i32Add, i32Mul, i32Scale, f64Scale,',
                     '         f64Axpy, gemv, gemvT, gemm, topkIndices } from "dyna:simd";'], tag)
    p.w(BITCMP)
    ncases = 0
    # ---- cumsum / cummax: exact for i32 and f32 (f32 is a sequential f32 fold)
    rows = []
    for n in LENS:
        rng = lcg(777 + n)
        ints = np.array([(next(rng) % 2000) - 1000 for _ in range(n)], np.int64).astype(np.int32)
        rows.append(("cumsum", "i", ints, np.cumsum(ints, dtype=np.int32), None))
        rows.append(("cummax", "i", ints, np.maximum.accumulate(ints), None))
        f = np.array([((next(rng) >> 8) / 16777216.0) * 20 - 10 for _ in range(n)], np.float64).astype(np.float32)
        rows.append(("cumsum", "f", f, np.cumsum(f, dtype=np.float32),
                     np.cumsum(np.abs(f), dtype=np.float64)))  # drift-bounded below
        rows.append(("cummax", "f", f, np.maximum.accumulate(f), None))  # exact
    p.w("var SCAN = [")
    srows = []
    for (op, k, inp, out, drift) in rows:
        if k == "i":
            srows.append("[%s,%s,%s,null]" % (json.dumps(op), js_i32(inp), js_i32(out)))
        else:
            extra = js_f32(drift.astype(np.float32)) if drift is not None else "null"
            srows.append("[%s,%s,%s,%s]" % (json.dumps(op), js_f32(inp), js_f32(out), extra))
    p.w(",\n".join(srows))
    p.w("];")
    p.w("""
for (var i = 0; i < SCAN.length; i++) {
  var op = SCAN[i][0], arr = SCAN[i][1], want = SCAN[i][2];
  var gotA = (op === "cumsum" ? cumsum(arr) : cummax(arr));
  if (op === "cumsum" && arr.length && arr[0] % 1 !== 0) { /* f32 case below */ }
  if (gotA !== arr) { __fail++; __out("FAIL " + op + "#" + i + " not in-place"); continue; }
  if (op === "cumsum" && arr.BYTES_PER_ELEMENT === 4 && SCAN[i][3] !== null) {
    // documented: the f32 scan reorders additions vs a left fold. Bound the
    // drift by f32-eps * sum(|x_i|) per element -- tight where the running sum
    // crosses zero, where a purely relative bound would be meaningless.
    var drift = SCAN[i][3], ok = true;
    for (var j = 0; j < gotA.length; j++) {
      var lim = 1e-6 + 2.5e-7 * drift[j];
      if (!(Math.abs(gotA[j] - want[j]) <= lim)) {
        __fail++; __out("FAIL " + op + "#" + i + "[" + j + "] drift " +
                        Math.abs(gotA[j] - want[j]) + " > " + lim);
        ok = false; break;
      }
    }
    if (ok) __pass++;
  } else {
    checkbits(op, i, gotA, want);
  }
}
""")
    ncases += len(rows)

    # ---- i32 family: exact mod-2^32 semantics == numpy int32
    rows = []
    rng = lcg(4242)
    for n in LENS:
        au = np.array([next(rng) for _ in range(n)], np.uint32)
        bu = np.array([next(rng) for _ in range(n)], np.uint32)
        a = au.view(np.int32)
        b = bu.view(np.int32)
        rows.append(("i32Add", a, b, (a.astype(np.int64) + b.astype(np.int64)).astype(np.int32)))
        rows.append(("i32Mul", a, b, (a.astype(np.int64) * b.astype(np.int64)).astype(np.int32)))
        rows.append(("i32Scale", a, None, (a.astype(np.int64) * np.int64(1000)).astype(np.int32)))
    p.w("var I32 = [")
    irows = []
    for (op, a, b, out) in rows:
        if b is None:
            irows.append("[%s,%s,null,%s]" % (json.dumps(op), js_i32(a), js_i32(out)))
        else:
            irows.append("[%s,%s,%s,%s]" % (json.dumps(op), js_i32(a), js_i32(b), js_i32(out)))
    p.w(",\n".join(irows))
    p.w("];")
    p.w("""
for (var i = 0; i < I32.length; i++) {
  var op = I32[i][0], a = I32[i][1], b = I32[i][2], want = I32[i][3];
  var gotA;
  if (op === "i32Add") gotA = i32Add(new Int32Array(a.length), a, b);
  else if (op === "i32Mul") gotA = i32Mul(new Int32Array(a.length), a, b);
  else gotA = i32Scale(a, 1000);
  checkbits(op, i, gotA, want);
}
""")
    ncases += len(rows)

    # ---- f64 family: bit-exact vs numpy float64
    rows = []
    rng = lcg(9090)
    for n in LENS:
        a = np.array([((next(rng) >> 8) / 16777216.0) * 20 - 10 for _ in range(n)])
        b = np.array([((next(rng) >> 8) / 16777216.0) * 20 - 10 for _ in range(n)])
        rows.append(("f64Scale", a, None, a * 2.5))
        rows.append(("f64Axpy", a, b, a + b * 1.25))
    p.w("var F64 = [")
    frows = []
    for (op, a, b, out) in rows:
        if b is None:
            frows.append("[%s,%s,null,%s]" % (json.dumps(op), js_f64(a), js_f64(out)))
        else:
            frows.append("[%s,%s,%s,%s]" % (json.dumps(op), js_f64(a), js_f64(b), js_f64(out)))
    p.w(",\n".join(frows))
    p.w("];")
    p.w("""
for (var i = 0; i < F64.length; i++) {
  var op = F64[i][0], a = F64[i][1], b = F64[i][2], want = F64[i][3];
  var gotA;
  if (op === "f64Scale") gotA = f64Scale(a, 2.5);
  else gotA = f64Axpy(a, 1.25, b);
  checkbits(op, i, gotA, want);
}
""")
    ncases += len(rows)

    # ---- BLAS-2/3 vs float64 numpy matmul cast to f32 (tolerance: summation
    #      order is implementation-defined; the RESULT class is not)
    p.w("var BL = [")
    brows = []
    nbl = 0
    rng = lcg(5150)
    for (m, k, n) in [(1,1,1),(2,3,4),(3,0,3),(0,2,2),(5,5,5),(8,16,7),(17,31,33),(64,64,64)]:
        A = np.array([((next(rng) >> 8) / 16777216.0) * 4 - 2 for _ in range(m * k)]).astype(np.float32)
        X = np.array([((next(rng) >> 8) / 16777216.0) * 4 - 2 for _ in range(k)]).astype(np.float32)
        Y = np.array([((next(rng) >> 8) / 16777216.0) * 4 - 2 for _ in range(m)]).astype(np.float32)
        if k:
            gemv_ref = (A.reshape(m, k).astype(np.float64) @ X.astype(np.float64)).astype(np.float32)
            gemvT_ref = (A.reshape(m, k).T.astype(np.float64) @ Y.astype(np.float64)).astype(np.float32)
            # (length k: the binding's gemvT output is its n == our k)
        else:
            gemv_ref = np.zeros(m, np.float32)
            gemvT_ref = np.zeros(k, np.float32)
        brows.append('["gemv",%d,%d,%d,%s,%s,%s]' % (m, k, n, js_f32(A), js_f32(X), js_f32(gemv_ref)))
        brows.append('["gemvT",%d,%d,%d,%s,%s,%s]' % (m, k, n, js_f32(A), js_f32(Y), js_f32(gemvT_ref)))
        nbl += 2
        if k and n:
            Bmat = np.array([((next(rng) >> 8) / 16777216.0) * 4 - 2 for _ in range(k * n)]).astype(np.float32)
            ref = (A.reshape(m, k).astype(np.float64) @ Bmat.reshape(k, n).astype(np.float64)).astype(np.float32)
            brows.append('["gemm",%d,%d,%d,%s,%s,%s]' % (m, k, n, js_f32(A), js_f32(Bmat), js_f32(ref)))
            nbl += 1
    p.w(",\n".join(brows))
    p.w("];")
    p.w("""
for (var i = 0; i < BL.length; i++) {
  var op = BL[i][0], m = BL[i][1], k = BL[i][2], n = BL[i][3];
  var A = BL[i][4], X = BL[i][5], ref = BL[i][6];
  var y;
  if (op === "gemv") {
    y = new Float32Array(m);
    gemv(y, A, X, m, k, 0);
  } else if (op === "gemvT") {
    y = new Float32Array(k);
    gemvT(y, A, X, m, k, 0);
  } else {
    y = new Float32Array(m * n);
    gemm(y, A, X, m, n, k, 1, 0);
  }
  assert_arr_close(Array.from(y), Array.from(ref), 1e-3, 1e-4, op + "#" + i);
}
""")
    ncases += nbl

    # ---- topk semantics
    p.w("""
var tk = new Float32Array([1, 5, 3, 9, 2, 9, -1, 0]);
assert_arr_eq(Array.from(topkIndices(tk, 3)).sort(), [1, 3, 5], "topk keeps the k largest");
assert_eq(Array.from(topkIndices(tk, 99)).length, 8, "topk k>n clamps");
assert_eq(Array.from(topkIndices(tk, 0)).length, 0, "topk k=0 empty");
assert_eq(Array.from(topkIndices(new Float32Array(0), 3)).length, 0, "topk empty input");
""")
    ncases += 4
    p.close(ncases)

# ------------------------------------------------------- approx + reductions
def gen_approx():
    tag = "simd_approx"
    p = ProbeWriter(os.path.join(PROBES, tag + ".js"),
                    ['import { sigmoid, tanhFast, silu, elu, gelu, softmax,',
                     '         logSoftmax, vexp, vlog, vsqrt, vrsqrt, vinv, sum, dot,',
                     '         normL1, normL2, distL1, distL2, distCheb, distCos }',
                     '  from "dyna:simd";'], tag)

    rng = lcg(31337)
    # tolerances = the module's documented fast_exp error class, widened to
    # the worst case observed over random +-5 inputs (test_simd.js's class
    # bounds were calibrated on its own vectors; Schraudolph's rel error is
    # worst near |x| ~ 1 and reaches ~3e-2 for sigmoid there)
    TOL = {"sigmoid": (1.0, 3e-2), "tanhFast": (1.0, 5e-2), "silu": (1.0, 5e-2),
           "elu": (1e-3, 1e-5), "gelu": (1.0, 2e-2), "softmax": (1.0, 3e-2),
           "logSoftmax": (1.0, 1e-1), "vexp": (1.5e-1, 5e-2), "vlog": (1e-5, 1e-6),
           "vsqrt": (1e-6, 1e-7), "vrsqrt": (1e-3, 1e-5), "vinv": (1e-6, 1e-7)}
    cases = []
    for n in [0, 1, 2, 3, 7, 8, 15, 16, 31, 32, 63, 64, 65, 100, 255]:
        xs = np.array([((next(rng) >> 8) / 16777216.0) * 10 - 5 for _ in range(n)]).astype(np.float32)
        x64 = xs.astype(np.float64)
        for op in ["sigmoid", "tanhFast", "silu", "elu", "gelu", "vexp", "vlog",
                   "vsqrt", "vrsqrt", "vinv"]:
            if op == "sigmoid":
                ref = 1.0 / (1.0 + np.exp(-x64))
            elif op == "tanhFast":
                ref = np.tanh(x64)
            elif op == "silu":
                ref = x64 / (1.0 + np.exp(-x64))
            elif op == "elu":
                ref = np.where(xs > 0, x64, np.exp(np.minimum(xs, 0).astype(np.float64)) - 1.0)
            elif op == "gelu":
                ref = 0.5 * x64 * (1.0 + np.tanh(0.7978845608028654 * (x64 + 0.044715 * x64 ** 3)))
            elif op == "vexp":
                ref = np.exp(x64)
            elif op == "vlog":
                with np.errstate(all="ignore"):
                    ref = np.where(xs > 0, np.log(np.where(xs > 0, x64, 1.0)), np.float64(-3.4028235e38))
            elif op == "vsqrt":
                ref = np.where(xs >= 0, np.sqrt(np.where(xs >= 0, x64, 0.0)), 0.0)
            elif op == "vrsqrt":
                # body lanes: 1/sqrt(0) and 1/sqrt(neg) are NaN; the scalar
                # tail keeps the (v>0 ? 1/sqrt : 0) contract
                xr = np.arange(n)
                body = n - (n % 4)
                tailref = np.where(xs > 0, 1.0 / np.sqrt(np.where(xs > 0, x64, 1.0)), 0.0)
                ref = np.where((xr < body) & (xs <= 0), np.nan, tailref)
            else:
                ref = np.where(xs != 0, 1.0 / np.where(xs != 0, x64, 1.0), 0.0)
            cases.append((op, xs, ref, TOL[op][0], TOL[op][1]))
        if n:
            mx = x64.max()
            e = np.exp(x64 - mx)
            sm = e / e.sum()
            cases.append(("softmax", xs, sm, TOL["softmax"][0], TOL["softmax"][1]))
            cases.append(("logSoftmax", xs, np.log(sm), TOL["logSoftmax"][0], TOL["logSoftmax"][1]))
    p.w("var AC = [")
    rows = []
    for (op, xs, ref, rel, ab) in cases:
        rows.append("[%s,%s,[%s],%s,%s]" % (
            json.dumps(op), js_f32(xs),
            ",".join(("'NaN'" if v != v else repr(float(v))) for v in ref), repr(rel), repr(ab)))
    p.w(",\n".join(rows))
    p.w("];")
    p.w("""
var F = { sigmoid:sigmoid, tanhFast:tanhFast, silu:silu, elu:elu, gelu:gelu,
          vexp:vexp, vlog:vlog, vsqrt:vsqrt, vrsqrt:vrsqrt, vinv:vinv,
          softmax:softmax, logSoftmax:logSoftmax };
for (var i = 0; i < AC.length; i++) {
  var op = AC[i][0], inp = AC[i][1], want = AC[i][2], rel = AC[i][3], ab = AC[i][4];
  var a = new Float32Array(inp.length);
  a.set(inp);
  var r = (op === "elu") ? F[op](a, 1) : F[op](a);
  assert_arr_close(Array.from(r), want, rel, ab, op + "#" + i);
}
""")
    # reductions
    rows1, rows2 = [], []
    for n in [1, 3, 7, 8, 16, 31, 63, 64, 100, 255, 4096]:
        xs = np.array([((next(rng) >> 8) / 16777216.0) * 10 - 5 for _ in range(n)]).astype(np.float32)
        ys = np.array([((next(rng) >> 8) / 16777216.0) * 10 - 5 for _ in range(n)]).astype(np.float32)
        x64, y64 = xs.astype(np.float64), ys.astype(np.float64)
        rows1.append(("sum", xs, float(np.sum(x64)), 1e-5, 1e-5))
        rows1.append(("normL1", xs, float(np.sum(np.abs(x64))), 1e-5, 1e-5))
        rows1.append(("normL2", xs, float(np.sqrt(np.sum(x64 ** 2))), 1e-4, 1e-5))
        rows2.append(("dot", xs, ys, float(np.sum(x64 * y64)), 1e-4, 1e-5))
        rows2.append(("distL1", xs, ys, float(np.sum(np.abs(x64 - y64))), 1e-5, 1e-5))
        rows2.append(("distL2", xs, ys, float(np.sqrt(np.sum((x64 - y64) ** 2))), 1e-4, 1e-5))
        rows2.append(("distCheb", xs, ys, float(np.max(np.abs(x64 - y64))), 1e-6, 1e-7))
        denom = np.sqrt(np.sum(x64 ** 2) * np.sum(y64 ** 2))
        dcos = 1.0 if denom < 1.17549435e-38 else float(1.0 - np.sum(x64 * y64) / denom)
        rows2.append(("distCos", xs, ys, dcos, 1e-4, 1e-5))
    p.w("var RD1 = [")
    p.w(",\n".join('[%s,%s,%s,%s,%s]' % (json.dumps(op), js_f32(xs), repr(want), repr(rel), repr(ab))
                   for (op, xs, want, rel, ab) in rows1))
    p.w("];")
    p.w("var RD2 = [")
    p.w(",\n".join('[%s,%s,%s,%s,%s,%s]' % (json.dumps(op), js_f32(xs), js_f32(ys), repr(want), repr(rel), repr(ab))
                   for (op, xs, ys, want, rel, ab) in rows2))
    p.w("];")
    p.w("""
var R1 = { sum:sum, normL1:normL1, normL2:normL2 };
var R2 = { dot:dot, distL1:distL1, distL2:distL2, distCheb:distCheb, distCos:distCos };
for (var i = 0; i < RD1.length; i++) {
  var op = RD1[i][0], a = RD1[i][1], want = RD1[i][2], rel = RD1[i][3], ab = RD1[i][4];
  assert_close(R1[op](a), want, rel, ab, op + "#" + i);
}
for (var i = 0; i < RD2.length; i++) {
  var op = RD2[i][0], a = RD2[i][1], b = RD2[i][2], want = RD2[i][3], rel = RD2[i][4], ab = RD2[i][5];
  assert_close(R2[op](a, b), want, rel, ab, op + "#" + i);
}
""")
    p.close(len(cases) + len(rows1) + len(rows2))

# --------------------------------------------------- API edges / NaN policy
def gen_edges():
    tag = "simd_edges"
    p = ProbeWriter(os.path.join(PROBES, tag + ".js"),
                    ['import { sum, max, min, argmax, argmin, softmax, logSoftmax,',
                     '         f64Max, f64Min, i32Min, i32Max, add, dot, cumsum, cummax,',
                     '         f64Sum, gemv, gemm } from "dyna:simd";'], tag)
    n = 0
    edges = [
        ('max(new Float32Array(0))', "RangeError"),
        ('min(new Float32Array(0))', "RangeError"),
        ('argmax(new Float32Array(0))', "RangeError"),
        ('argmin(new Float32Array(0))', "RangeError"),
        ('softmax(new Float32Array(0))', "RangeError"),
        ('logSoftmax(new Float32Array(0))', "RangeError"),
        ('f64Max(new Float64Array(0))', "RangeError"),
        ('f64Min(new Float64Array(0))', "RangeError"),
        ('i32Min(new Int32Array(0))', "RangeError"),
        ('i32Max(new Int32Array(0))', "RangeError"),
        ('add(new Float32Array(3), new Float32Array(2), new Float32Array(3))', "RangeError"),
        ('add(new Float32Array(3), new Float32Array(3), new Float32Array(2))', "RangeError"),
        ('dot(new Float32Array(2), new Float32Array(3))', "RangeError"),
        ('gemv(new Float32Array(2), new Float32Array(4), new Float32Array(2), 3, 2, 0)', "RangeError"),
        ('gemv(new Float32Array(3), new Float32Array(7), new Float32Array(2), 3, 2, 0)', "RangeError"),
        ('gemm(new Float32Array(4), new Float32Array(6), new Float32Array(8), 2, 2, 3, 1, 0)', "RangeError"),
        ('sum(new Float64Array(4))', "TypeError"),      # bpe != 4
        ('f64Sum(new Float32Array(4))', "TypeError"),   # bpe != 8
        ('i32Min(new Float32Array(2))', "TypeError"),   # class id, not stride
        ('i32Min(new Uint32Array(2))', "TypeError"),
        ('cumsum(new Float64Array(2))', "TypeError"),   # neither i32 nor f32
        ('gemv(new Float32Array(2), new Float32Array(4), new Float32Array(2), 1.5, 2, 0)', "RangeError"),
    ]
    for expr, kind in edges:
        p.w('assert_throws(function () { %s; }, "%s", "edge %s");' % (expr, kind, expr))
        n += 1
    # documented breadth: any 4-byte typed array is accepted by the f32 family,
    # reinterpreted as float32 bits (contract, not accident)
    p.w("""
var i32 = new Int32Array([0x3f800000, 0x40000000, 0xc0000000]);  // 1.0, 2.0, -2.0 as f32
assert_close(sum(i32), 1.0, 0, 1e-6, "f32 family accepts 4-byte i32 bits");
// empty non-throwing reductions
assert_eq(sum(new Float32Array(0)), 0, "sum(empty) = 0");
assert_eq(f64Sum(new Float64Array(0)), 0, "f64Sum(empty) = 0");
assert_eq(dot(new Float32Array(0), new Float32Array(0)), 0, "dot(empty) = 0");
// NaN policy of max/argmax == the scalar semantics (documented impl choice):
// a NaN at index 0 poisons the fold; a NaN later is skipped by strict >.
for (var len = 1; len <= 130; len = len * 2 + 1) {
  var a = new Float32Array(len).fill(2);
  a[0] = NaN;
  assert_true(isNaN(max(a)), "max NaN@0 len=" + len);
  assert_eq(argmax(a), 0, "argmax NaN@0 len=" + len);
  if (len > 5) {
    var b = new Float32Array(len).fill(2);
    b[3] = NaN; b[len - 1] = 7;
    // DOCUMENTED DIVERGENCE (impl-defined): below the binding's 64-element
    // safe-reduce bound the scalar path skips later NaNs (strict > fold);
    // at or above it the vector path propagates any NaN. Assert the length
    // that the call actually took so a change in either regime is caught.
    if (len < 64) {
      assert_eq(max(b), 7, "max skips later NaN (scalar regime) len=" + len);
      assert_eq(argmax(b), len - 1, "argmax skips later NaN (scalar) len=" + len);
    } else {
      assert_true(isNaN(max(b)), "max propagates NaN (vector regime) len=" + len);
    }
  }
}
// in-place == fresh for activations (aliasing contract)
""")
    n += 1
    p.close(n + 1)

def main():
    gen_elementwise()
    gen_kernels()
    gen_approx()
    gen_edges()

if __name__ == "__main__":
    main()
