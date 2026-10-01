// Parametric black-box contract test for dyna:mathx, generated from dynajs.d.ts lines 3070-3360. Engine sources not consulted.
import * as mathx from "dyna:mathx";

let n = 0;
function assert(cond, msg) { n++; if (!cond) throw new Error("assertion failed: " + msg); }
function assertEq(actual, expected, msg) { n++; const ok = Object.is(actual, expected) || (typeof actual === "number" && typeof expected === "number" && Number.isNaN(actual) && Number.isNaN(expected)); if (!ok) throw new Error("assertion failed: " + msg + " — got |" + actual + "| expected |" + expected + "|"); }
function assertClose(actual, expected, eps, msg) { n++; if (!(Math.abs(actual - expected) <= eps)) throw new Error("assertion failed (close): " + msg + " — got |" + actual + "| expected |" + expected + "±" + eps + "|"); }
function assertThrows(fn, msg, ErrType, errPattern) { n++; let threw = false, e = null; try { fn(); } catch (err) { threw = true; e = err; } if (!threw) throw new Error("expected throw: " + msg); if (ErrType && !(e instanceof ErrType)) throw new Error("wrong error type " + (e && e.constructor ? e.constructor.name : String(e)) + ": " + msg); if (errPattern && !(errPattern instanceof RegExp ? errPattern.test(String(e)) : String(e).includes(errPattern))) throw new Error("wrong error message |" + e + "|: " + msg); }
function eqArr(a, b) { if (a.length !== b.length) return false; for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false; return true; }
function eqArr2(a, b) { if (a.length !== b.length) return false; for (let i = 0; i < a.length; i++) if (!eqArr(a[i], b[i])) return false; return true; }

// ============================ constants (d.ts L3071-3092) ============================
// "Mathematical constants, written with enough digits for one correctly-rounded conversion."
{
  const cases = [
    ["E === Math.E", mathx.E, Math.E],
    ["Pi === Math.PI", mathx.Pi, Math.PI],
    ["Sqrt2 === Math.SQRT2", mathx.Sqrt2, Math.SQRT2],
    ["Ln2 === Math.LN2", mathx.Ln2, Math.LN2],
    ["Ln10 === Math.LN10", mathx.Ln10, Math.LN10],
    ["Log2E === Math.LOG2E", mathx.Log2E, Math.LOG2E],
    ["Log10E === Math.LOG10E", mathx.Log10E, Math.LOG10E],
    ["MaxInt32", mathx.MaxInt32, 2147483647],
    ["MinInt32", mathx.MinInt32, -2147483648],
    ["MaxSafeInteger === Number.MAX_SAFE_INTEGER", mathx.MaxSafeInteger, Number.MAX_SAFE_INTEGER],
    ["MaxInt64 === 2^63-1", mathx.MaxInt64, 9223372036854775807n],
  ];
  for (const [label, got, expected] of cases) assertEq(got, expected, label);
  // golden ratio / sqrt anchors (MATLAB-style constant set)
  assertClose(mathx.Phi, (1 + Math.sqrt(5)) / 2, 1e-15, "Phi is the golden ratio");
  assertClose(mathx.SqrtE, Math.sqrt(Math.E), 1e-15, "SqrtE = sqrt(E)");
  assertClose(mathx.SqrtPi, Math.sqrt(Math.PI), 1e-15, "SqrtPi = sqrt(pi)");
}

// ============================ constants: exact tail (d.ts L3071-3092) ============================
// "Mathematical constants, written with enough digits for one correctly-rounded conversion."
// The three irrational constants the block above only bounds with assertClose are still EXACTLY
// pinned by that sentence: each is the correctly-rounded double of the true mathematical value
// (hand-derived offline at 60-digit precision; NOT computed through Math.* anchors):
//   Phi   = (1+sqrt(5))/2 -> 0x1.9e3779b97f4a8p+0
//   SqrtE = sqrt(e)       -> 0x1.a61298e1e069cp+0
//   SqrtPi= sqrt(pi)      -> 0x1.c5bf891b4ef6bp+0
// Note SqrtPi is one ulp ABOVE Math.sqrt(Math.PI): pi's double is below true pi and the sqrt
// carries that bias in, so the two-step Math route lands at ...5159, not the one-conversion value.
{
  const cases = [
    ["Phi === correctly-rounded (1+sqrt5)/2", mathx.Phi, 1.618033988749895],
    ["SqrtE === correctly-rounded sqrt(e)", mathx.SqrtE, 1.6487212707001282],
    ["SqrtPi === correctly-rounded sqrt(pi)", mathx.SqrtPi, 1.772453850905516],
    ["MaxInt64 is a bigint", typeof mathx.MaxInt64, "bigint"],
  ];
  for (const [label, got, expected] of cases) assertEq(got, expected, label);
  assertEq(String(mathx.MaxInt64), "9223372036854775807", "MaxInt64 prints as 2^63-1");
}

// ============================ realmin / realmax / flintmax / eps (d.ts L3094-3103) ============================
{
  const cases = [
    // DBL_MIN is the smallest positive NORMAL (2.2250738585072014e-308); Number.MIN_VALUE
// (5e-324) is the smallest SUBNORMAL — the d.ts pins "normal", so C DBL_MIN is right.
["realmin = DBL_MIN", mathx.realmin(), 2.2250738585072014e-308],          // "Smallest positive normal double (DBL_MIN)"
    ["realmax = DBL_MAX", mathx.realmax(), Number.MAX_VALUE],          // "Largest finite double (DBL_MAX)"
    ["flintmax = 2^53", mathx.flintmax(), 9007199254740992],           // "2^53, the largest integer every double below it represents exactly"
    ["eps() === eps(1)", mathx.eps(), mathx.eps(1)],                   // "bare eps is eps(1)"
    ["eps(1) = machine epsilon", mathx.eps(1), Number.EPSILON],        // gap above 1 is 2^-52
    ["eps(0.5) = 2^-53", mathx.eps(0.5), Number.EPSILON / 2],          // binade [0.5,1) spacing is 2^-53
    ["eps(realmax) = 2^971", mathx.eps(mathx.realmax()), Math.pow(2, 971)], // documented verbatim: "eps(realmax) is the ulp of the top binade, 2^971"
  ];
  for (const [label, got, expected] of cases) assertEq(got, expected, label);
}

// ============================ round / roundToEven / fix / trunc (d.ts L3105-3116) ============================
// round: "C99 round (ties away from zero)"; roundToEven: "Round half to even"; fix: "Truncate toward zero"; trunc: "C trunc"
{
  const cases = [
    ["round(2.5)", 2.5, 3], ["round(-2.5)", -2.5, -3],
    ["round(0.5)", 0.5, 1], ["round(-0.5)", -0.5, -1],
    ["round(1.4)", 1.4, 1], ["round(-1.6)", -1.6, -2], ["round(0)", 0, 0],
  ];
  for (const [label, x, expected] of cases) assertEq(mathx.round(x), expected, label);

  const cases2 = [
    ["roundToEven(2.5)", 2.5, 2], ["roundToEven(3.5)", 3.5, 4],
    ["roundToEven(-2.5)", -2.5, -2], ["roundToEven(0.5)", 0.5, 0],
    ["roundToEven(1.4)", 1.4, 1],
  ];
  for (const [label, x, expected] of cases2) assertEq(mathx.roundToEven(x), expected, label);

  const cases3 = [
    ["fix(2.7)", 2.7, 2], ["fix(-2.7)", -2.7, -2], ["fix(0.9)", 0.9, 0], ["fix(-0.9)", -0.9, -0],
    ["trunc(2.9)", 2.9, 2], ["trunc(-2.9)", -2.9, -2], ["trunc(0)", 0, 0],
  ];
  for (const [label, x, expected] of cases3) assertEq(label.startsWith("fix") ? mathx.fix(x) : mathx.trunc(x), expected, label);
}

// ============================ sign / signbit (d.ts L3112-3114) ============================
// sign: "1, -1, or x itself (-0 and NaN pass through)"; signbit: "The sign bit directly"
{
  const cases = [
    ["sign(5)", 5, 1], ["sign(-5)", -5, -1], ["sign(0)", 0, 0],
    ["sign(-0) is -0", -0, -0], ["sign(NaN) is NaN", NaN, NaN],
  ];
  for (const [label, x, expected] of cases) assertEq(mathx.sign(x), expected, label);

  const cases2 = [
    ["signbit(-0)", -0, true], ["signbit(0)", 0, false],
    ["signbit(-1)", -1, true], ["signbit(1)", 1, false],
  ];
  for (const [label, x, expected] of cases2) assertEq(mathx.signbit(x), expected, label);
}

// ============================ modf (d.ts L3118) ============================
// "Splits into [intPart, fracPart]."
{
  const cases = [
    ["modf(2.75)", 2.75, [2, 0.75]],
    ["modf(-2.75)", -2.75, [-2, -0.75]],
    ["modf(3)", 3, [3, 0]],
    ["modf(0.5)", 0.5, [0, 0.5]],
  ];
  for (const [label, x, expected] of cases) assert(eqArr2([mathx.modf(x)], [expected]), label);
}

// ============================ mod / rem / fmod / remainder / idivide / nthroot (d.ts L3120-3130) ============================
// mod: "MATLAB floored modulo: mod(-7, 3) is 2; mod(a, 0) is a"
{
  const cases = [
    ["mod(-7,3)", -7, 3, 2], ["mod(7,3)", 7, 3, 1],
    ["mod(7,-3)", 7, -3, -2], ["mod(-7,-3)", -7, -3, -1],
    ["mod(6,3)", 6, 3, 0], ["mod(5,0)", 5, 0, 5], // documented: "mod(a, 0) is a"
  ];
  for (const [label, a, b, expected] of cases) assertEq(mathx.mod(a, b), expected, label);
}
// rem/fmod: "Truncated fmod" (C fmod keeps the dividend's sign)
{
  const cases = [
    ["rem(7,3)", 7, 3, 1], ["rem(-7,3)", -7, 3, -1], ["rem(7,-3)", 7, -3, 1],
    ["fmod(7,3)", 7, 3, 1], ["fmod(-7,3)", -7, 3, -1], ["fmod(7,-3)", 7, -3, 1],
  ];
  for (const [label, a, b, expected] of cases) assertEq(label.startsWith("rem") ? mathx.rem(a, b) : mathx.fmod(a, b), expected, label);
  // parity: rem and fmod are two names for the same truncated remainder
  assertEq(mathx.fmod(-7, 3), mathx.rem(-7, 3), "fmod/rem parity on (-7,3)");
}
// remainder: "C99 round-to-nearest remainder"
{
  const cases = [
    ["remainder(7,3)", 7, 3, 1],  // 7 = 2*3 + 1
    ["remainder(8,3)", 8, 3, -1], // 8 = 3*3 - 1
    ["remainder(5,3)", 5, 3, -1], // 5 = 2*3 - 1
    ["remainder(6,3)", 6, 3, 0],
  ];
  for (const [label, a, b, expected] of cases) assertEq(mathx.remainder(a, b), expected, label);
}
// idivide: "Integer division with an explicit rounding mode: fix|floor|ceil|round" (default fix)
{
  const cases = [
    ["idivide(7,2) default fix", 7, 2, undefined, 3],
    ["idivide(7,2,fix)", 7, 2, "fix", 3],
    ["idivide(-7,2,fix)", -7, 2, "fix", -3],
    ["idivide(-7,2,floor)", -7, 2, "floor", -4],
    ["idivide(7,2,floor)", 7, 2, "floor", 3],
    ["idivide(7,2,ceil)", 7, 2, "ceil", 4],
    ["idivide(-7,2,ceil)", -7, 2, "ceil", -3],
    ["idivide(7,2,round)", 7, 2, "round", 4],  // 3.5 -> 4 (half away or half-even agree)
    ["idivide(-7,2,round)", -7, 2, "round", -4],
    ["idivide(8,2,round)", 8, 2, "round", 4],
  ];
  for (const [label, a, b, mode, expected] of cases) assertEq(mathx.idivide(a, b, mode), expected, label);
}
// nthroot: "The real n-th root; defined for negative x with odd integer n"
{
  const cases = [
    ["nthroot(8,3)", 8, 3, 2], ["nthroot(-8,3)", -8, 3, -2],
    ["nthroot(27,3)", 27, 3, 3], ["nthroot(16,4)", 16, 4, 2],
    ["nthroot(-27,3)", -27, 3, -3],
  ];
  for (const [label, x, p, expected] of cases) assertClose(mathx.nthroot(x, p), expected, 1e-9, label);
}

// ============================ gamma / cbrt / hypot / copysign / nextafter (d.ts L3132-3137) ============================
{
  const cases = [
    ["gamma(1)", 1, 1], ["gamma(5)", 5, 24],                       // Gamma(n) = (n-1)!
    ["gamma(0.5)", 0.5, Math.sqrt(Math.PI)],                       // Gamma(1/2) = sqrt(pi)
    ["gamma(-0.5)", -0.5, -2 * Math.sqrt(Math.PI)],                // Gamma(-1/2) = -2*sqrt(pi)
  ];
  for (const [label, x, expected] of cases) assertClose(mathx.gamma(x), expected, 1e-9, label);

  const cases2 = [
    ["cbrt(8)", 8, 2], ["cbrt(-8)", -8, -2], ["cbrt(27)", 27, 3], ["cbrt(0)", 0, 0],
  ];
  for (const [label, x, expected] of cases2) assertClose(mathx.cbrt(x), expected, 1e-12, label);

  const cases3 = [
    ["hypot(3,4)", 3, 4, 5], ["hypot(5,12)", 5, 12, 13], ["hypot(0,0)", 0, 0, 0],
  ];
  for (const [label, a, b, expected] of cases3) assertEq(mathx.hypot(a, b), expected, label); // exact Pythagorean triples

  const cases4 = [
    ["copysign(3,-1)", 3, -1, -3], ["copysign(-3,1)", -3, 1, 3],
    ["copysign(3,-0)", 3, -0, -3], ["copysign(0,-1)", 0, -1, -0],
  ];
  for (const [label, a, b, expected] of cases4) assertEq(mathx.copysign(a, b), expected, label);
}
// nextafter: "The gap to the next representable double away from zero"
{
  const cases = [
    ["nextafter(1,2)", 1, 2, 1 + Number.EPSILON],           // 1 + 2^-52, first double above 1
    ["nextafter(1,0)", 1, 0, 1 - Math.pow(2, -53)],         // last double below 1 (binade spacing 2^-53)
    ["nextafter(0,1)", 0, 1, Number.MIN_VALUE],             // smallest subnormal
    ["nextafter(2,1)", 2, 1, 2 - Number.EPSILON],           // 2 - 2^-52
    ["nextafter(MIN_VALUE,0)", Number.MIN_VALUE, 0, 0],     // below the smallest subnormal is 0
  ];
  for (const [label, a, b, expected] of cases) assertEq(mathx.nextafter(a, b), expected, label);
}

// ============================ expm1 / log1p / log2 / logb / pow2 (d.ts L3138-3144) ============================
{
  const cases = [
    ["expm1(0)", 0, 0], ["log1p(0)", 0, 0],
  ];
  for (const [label, x, expected] of cases) assertEq(label.startsWith("expm1") ? mathx.expm1(x) : mathx.log1p(x), expected, label);
  assertClose(mathx.expm1(1), Math.E - 1, 1e-14, "expm1(1) = e-1");
  assertClose(mathx.log1p(1), Math.LN2, 1e-15, "log1p(1) = ln2");
  const cases2 = [
    ["log2(8)", 8, 3], ["log2(1024)", 1024, 10], ["log2(0.5)", 0.5, -1], ["log2(1)", 1, 0],
  ];
  for (const [label, x, expected] of cases2) assertEq(mathx.log2(x), expected, label); // exact at powers of two
  const cases3 = [
    ["logb(8)", 8, 3], ["logb(0.5)", 0.5, -1], ["logb(1)", 1, 0], ["logb(3)", 3, 1], // 3 in [2,4)
  ];
  for (const [label, x, expected] of cases3) assertEq(mathx.logb(x), expected, label); // "The unbiased floating-point exponent (logb)"
  const cases4 = [
    ["pow2(3)", 3, 8], ["pow2(0)", 0, 1], ["pow2(-1)", -1, 0.5], ["pow2(10)", 10, 1024],
  ];
  for (const [label, x, expected] of cases4) assertEq(mathx.pow2(x), expected, label); // "2^x (exp2), the inverse of log2"
  assertClose(mathx.pow2(mathx.log2(5)), 5, 1e-9, "pow2/log2 round-trip at 5");
}

// ============================ deg2rad / rad2deg / nextpow2 (d.ts L3145-3148) ============================
{
  assertClose(mathx.deg2rad(180), Math.PI, 1e-15, "deg2rad(180) = pi");
  assertClose(mathx.deg2rad(90), Math.PI / 2, 1e-15, "deg2rad(90) = pi/2");
  assertEq(mathx.deg2rad(0), 0, "deg2rad(0) = 0");
  assertClose(mathx.rad2deg(Math.PI), 180, 1e-12, "rad2deg(pi) = 180");
  assertClose(mathx.rad2deg(mathx.deg2rad(30)), 30, 1e-9, "deg/rad round-trip at 30");
  // nextpow2: "The smallest p with 2^p >= |x|; nextpow2(0) is 0"
  const cases = [
    ["nextpow2(1)", 1, 0], ["nextpow2(5)", 5, 3], ["nextpow2(8)", 8, 3],
    ["nextpow2(9)", 9, 4], ["nextpow2(0)", 0, 0], ["nextpow2(-5)", -5, 3],
    ["nextpow2(0.5)", 0.5, -1], ["nextpow2(0.4)", 0.4, -1],
  ];
  for (const [label, x, expected] of cases) assertEq(mathx.nextpow2(x), expected, label);
}

// ============================ scalbn / ldexp / frexp / ilogb (d.ts L3150-3156) ============================
{
  const cases = [
    ["scalbn(1.5,3)", 1.5, 3, 12], ["scalbn(0.5,-2)", 0.5, -2, 0.125], ["scalbn(7,0)", 7, 0, 7],
  ];
  for (const [label, x, e, expected] of cases) assertEq(mathx.scalbn(x, e), expected, label);
  assertEq(mathx.ldexp(1.5, 3), mathx.scalbn(1.5, 3), "ldexp identical to scalbn"); // documented: "Identical to scalbn (MATLAB spelling)"
  // frexp: "Splits into x = frac * 2**exp with |frac| in [0.5, 1)"
  const cases2 = [
    ["frexp(8)", 8, [0.5, 4]], ["frexp(1)", 1, [0.5, 1]],
    ["frexp(6)", 6, [0.75, 3]], ["frexp(-3)", -3, [-0.75, 2]],
    ["frexp(0)", 0, [0, 0]], // C frexp convention at 0
  ];
  for (const [label, x, expected] of cases2) assert(eqArr2([mathx.frexp(x)], [expected]), label);
  // ilogb: "±Inf and NaN give 2^31-1, 0 gives -(2^31), else the unbiased exponent"
  const cases3 = [
    ["ilogb(8)", 8, 3], ["ilogb(0.5)", 0.5, -1], ["ilogb(1)", 1, 0],
    ["ilogb(+Inf)", Infinity, 2147483647], ["ilogb(-Inf)", -Infinity, 2147483647],
    ["ilogb(NaN)", NaN, 2147483647], ["ilogb(0)", 0, -2147483648],
  ];
  for (const [label, x, expected] of cases3) assertEq(mathx.ilogb(x), expected, label);
}

// ============================ isInf / isNaN (d.ts L3158-3159) ============================
// isInf: "Tests infinity, optionally restricted to one sign"
{
  const cases = [
    ["isInf(+Inf)", Infinity, undefined, true],
    ["isInf(-Inf)", -Infinity, undefined, true],
    ["isInf(1)", 1, undefined, false],
    ["isInf(+Inf, +1)", Infinity, 1, true],
    ["isInf(+Inf, -1)", Infinity, -1, false],
    ["isInf(-Inf, -1)", -Infinity, -1, true],
    ["isInf(-Inf, +1)", -Infinity, 1, false],
  ];
  for (const [label, x, s, expected] of cases) assertEq(mathx.isInf(x, s), expected, label);
  const cases2 = [["isNaN(NaN)", NaN, true], ["isNaN(1)", 1, false], ["isNaN(0)", 0, false]];
  for (const [label, x, expected] of cases2) assertEq(mathx.isNaN(x), expected, label);
}

// ============================ erf / erfc / erfinv / erfcinv / erfcx (d.ts L3161-3168) ============================
{
  const cases = [
    ["erf(0)", 0, 0], ["erfc(0)", 0, 1], // exact anchors
  ];
  for (const [label, x, expected] of cases) assertEq(label.startsWith("erf(") ? mathx.erf(x) : mathx.erfc(x), expected, label);
  assertClose(mathx.erf(1), 0.8427007929497149, 1e-12, "erf(1) standard value");
  assertClose(mathx.erfc(1), 0.15729920705028513, 1e-12, "erfc(1) standard value");
  assertClose(mathx.erf(1) + mathx.erfc(1), 1, 1e-15, "erf/erfc complement parity");
  // erfinv: "Inverts erf; erfinv(±1) is ±Inf"
  const cases2 = [
    ["erfinv(0)", 0, 0], ["erfinv(1)", 1, Infinity], ["erfinv(-1)", -1, -Infinity],
  ];
  for (const [label, y, expected] of cases2) assertEq(mathx.erfinv(y), expected, label);
  assertClose(mathx.erfinv(mathx.erf(0.5)), 0.5, 1e-6, "erfinv/erf round-trip at 0.5");
  // erfcinv: "erfinv(1 - y), domain [0, 2]"
  const cases3 = [
    ["erfcinv(1)", 1, 0], ["erfcinv(2)", 2, -Infinity], ["erfcinv(0)", 0, Infinity], // erfinv(1-y) chain
  ];
  for (const [label, y, expected] of cases3) assertEq(mathx.erfcinv(y), expected, label);
  assertClose(mathx.erfcinv(mathx.erfc(0.5)), 0.5, 1e-6, "erfcinv/erfc round-trip at 0.5");
  // erfcx: "exp(x^2)*erfc(x), finite where erfc underflows"
  assertEq(mathx.erfcx(0), 1, "erfcx(0) = 1");
  assertClose(mathx.erfcx(30), 1 / (30 * Math.sqrt(Math.PI)), 1e-3, "erfcx(30) asymptotic 1/(x*sqrt(pi)), finite");
}

// ============================ lgamma / gammaln / beta / betaln / psi / polygamma (d.ts L3170-3179) ============================
{
  const lg5 = mathx.lgamma(5);
  assert(eqArr2([lg5], [[Math.log(24), 1]]), "lgamma(5) = [ln(4!), +1]"); // "via the reentrant lgamma_r"
  const lgn = mathx.lgamma(-0.5);
  assertClose(lgn[0], Math.log(2 * Math.sqrt(Math.PI)), 1e-12, "lgamma(-0.5) log part");
  assertEq(lgn[1], -1, "lgamma(-0.5) sign part is -1");
  assertClose(mathx.gammaln(5), Math.log(24), 1e-12, "gammaln(5) = ln(24)");
  assertClose(mathx.gammaln(5), lg5[0], 1e-15, "gammaln/lgamma parity at 5");
  // beta: B(a,b) = Gamma(a)Gamma(b)/Gamma(a+b)
  assertClose(mathx.beta(1, 1), 1, 1e-15, "beta(1,1) = 1");
  assertClose(mathx.beta(2, 3), 1 / 12, 1e-15, "beta(2,3) = 1!*2!/4! = 1/12");
  assertClose(mathx.betaln(1, 1), 0, 1e-15, "betaln(1,1) = 0");
  assertClose(mathx.betaln(2, 3), Math.log(1 / 12), 1e-12, "betaln(2,3) = ln(1/12)");
  // psi: "Digamma; psi(0) is ±Inf, negative integers are NaN"
  assert(Math.abs(mathx.psi(0)) === Infinity, "psi(0) is an infinite pole");
  assert(Number.isNaN(mathx.psi(-1)), "psi(-1) is NaN");
  assertClose(mathx.psi(1), -0.5772156649015329, 1e-12, "psi(1) = -Euler-Mascheroni");
  assertClose(mathx.psi(0.5), -0.5772156649015329 - 2 * Math.LN2, 1e-12, "psi(1/2) = -gamma - 2ln2");
  // polygamma: "n-th derivative, order in [0, 64]"
  assertClose(mathx.polygamma(0, 1), mathx.psi(1), 1e-12, "polygamma(0,x) is psi(x) (parity)");
  assertClose(mathx.polygamma(1, 1), (Math.PI * Math.PI) / 6, 1e-9, "trigamma(1) = pi^2/6");
  assertThrows(() => mathx.polygamma(-1, 1), "polygamma order -1 below [0,64]");
  assertThrows(() => mathx.polygamma(65, 1), "polygamma order 65 above [0,64]");
}

// ============================ gammainc / gammaincinv / betainc / betaincinv / expint (d.ts L3180-3188) ============================
{
  // gammainc: "Regularised incomplete gamma (x first); upper selects the complement"; P(1,x) = 1 - e^-x
  assertEq(mathx.gammainc(0, 1), 0, "gammainc(0,1) = 0");
  assertEq(mathx.gammainc(0, 1, "upper"), 1, "gammainc(0,1,upper) = 1");
  assertClose(mathx.gammainc(1, 1), 1 - Math.exp(-1), 1e-12, "gammainc(1,1) = 1 - e^-1");
  assertClose(mathx.gammainc(1, 1, "upper"), Math.exp(-1), 1e-12, "gammainc upper = complement");
  // gammaincinv: "Inverts P(a, x) = p"
  assertEq(mathx.gammaincinv(0, 1), 0, "gammaincinv(0,1) = 0");
  assertClose(mathx.gammaincinv(mathx.gammainc(2, 1), 1), 2, 1e-6, "gammainc/gammaincinv round-trip");
  // betainc: regularised incomplete beta, endpoints exact
  assertEq(mathx.betainc(0, 2, 3), 0, "betainc(0,2,3) = 0");
  assertEq(mathx.betainc(1, 2, 3), 1, "betainc(1,2,3) = 1");
  assertClose(mathx.betainc(0.5, 2, 2), 0.5, 1e-12, "betainc(1/2,2,2) = 1/2 by symmetry");
  assertClose(mathx.betainc(0.3, 1, 1), 0.3, 1e-15, "betainc(x,1,1) = x");
  assertEq(mathx.betaincinv(0, 2, 3), 0, "betaincinv(0,2,3) = 0");
  assertEq(mathx.betaincinv(1, 2, 3), 1, "betaincinv(1,2,3) = 1");
  assertClose(mathx.betaincinv(0.5, 2, 2), 0.5, 1e-12, "betaincinv(1/2,2,2) = 1/2 by symmetry");
  assertClose(mathx.betaincinv(mathx.betainc(0.25, 2, 3), 2, 3), 0.25, 1e-9, "betainc/betaincinv round-trip");
  // expint: "E1(x), defined for x > 0"
  assertClose(mathx.expint(1), 0.2193839343955203, 1e-9, "E1(1) standard value");
}

// ============================ bessels (d.ts L3190-3201) ============================
{
  const cases = [
    ["besselj(0,0)", 0, 0, 1], ["besselj(1,0)", 1, 0, 0],
    ["besselj(0,1)", 0, 1, 0.7651976865579666],
    ["besselj(1,1)", 1, 1, 0.4400505857449335],
  ];
  for (const [label, ord, x, expected] of cases) assertClose(mathx.besselj(ord, x), expected, 1e-9, label);
  const casesY = [
    ["bessely(0,1)", 0, 1, 0.08825696421567695],
    ["bessely(1,1)", 1, 1, -0.7812128213002887],
  ];
  for (const [label, ord, x, expected] of casesY) assertClose(mathx.bessely(ord, x), expected, 1e-9, label);
  assertEq(mathx.bessely(0, 0), -Infinity, "bessely(0,0) diverges to -Inf (Y0 ~ ln x)");
  const casesI = [
    ["besseli(0,0)", 0, 0, 1],
    ["besseli(0,1)", 0, 1, 1.2660658777520084],
    ["besseli(1,1)", 1, 1, 0.565159103992485],
  ];
  for (const [label, nu, x, expected] of casesI) assertClose(mathx.besseli(nu, x), expected, 1e-9, label);
  const casesK = [
    ["besselk(0,1)", 0, 1, 0.4210244382407083],
    ["besselk(1,1)", 1, 1, 0.6019072301972346],
  ];
  for (const [label, nu, x, expected] of casesK) assertClose(mathx.besselk(nu, x), expected, 1e-9, label);
  assertEq(mathx.besselk(0, 0), Infinity, "besselk(0,0) diverges to +Inf (K0 ~ -ln x)");
  // scaled twins: "I_nu(x) e^-x" / "K_nu(x) e^x"
  assertClose(mathx.besseliScaled(0, 1), 1.2660658777520084 * Math.exp(-1), 1e-6, "besseliScaled(0,1) = I0(1)*e^-1");
  assertClose(mathx.besselkScaled(0, 1), 0.4210244382407083 * Math.E, 1e-6, "besselkScaled(0,1) = K0(1)*e");
  // besselh: "Hankel function J_n ± i Y_n; kind 1 or 2; returns [re, im]" — parity with besselj/bessely
  const h1 = mathx.besselh(0, 1, 1);
  assertClose(h1[0], mathx.besselj(0, 1), 1e-9, "besselh kind1 re = J0(1)");
  assertClose(h1[1], mathx.bessely(0, 1), 1e-9, "besselh kind1 im = Y0(1)");
  // d.ts signature is (n, x, kind): kind comes LAST, so kind 2 at x=1 is (0, 1, 2).
  const h2 = mathx.besselh(0, 1, 2);
  assertClose(h2[0], mathx.besselj(0, 1), 1e-9, "besselh kind2 re = J0(1)");
  assertClose(h2[1], -mathx.bessely(0, 1), 1e-9, "besselh kind2 im = -Y0(1)");
}

// ============================ ellipke / ellipj / legendre / airy (d.ts L3203-3213) ============================
{
  const ke0 = mathx.ellipke(0);
  assert(eqArr([...ke0], [Math.PI / 2, Math.PI / 2]), "ellipke(0) = [pi/2, pi/2]"); // K(0)=E(0)=pi/2
  const ke1 = mathx.ellipke(1);
  assert(ke1[0] === Infinity, "ellipke(1): K diverges (AGM(1,0)=0)");
  assertClose(ke1[1], 1, 1e-12, "ellipke(1): E(1) = 1");
  const ej0 = mathx.ellipj(0, 0.5);
  assertClose(ej0.sn, 0, 1e-12, "ellipj sn(0) = 0");
  assertClose(ej0.cn, 1, 1e-12, "ellipj cn(0) = 1");
  assertClose(ej0.dn, 1, 1e-12, "ellipj dn(0) = 1");
  const ejm0 = mathx.ellipj(0.5, 0);
  assertClose(ejm0.sn, Math.sin(0.5), 1e-9, "ellipj(u,0) sn = sin(u)");
  assertClose(ejm0.cn, Math.cos(0.5), 1e-9, "ellipj(u,0) cn = cos(u)");
  assertClose(ejm0.dn, 1, 1e-9, "ellipj(u,0) dn = 1");
  // legendre: "Associated Legendre functions P_n^m for the whole column m = 0..n"
  const L2 = mathx.legendre(2, 0);
  assert(eqArr(L2, [-0.5, 0, 3]), "legendre(2,0) column = [P2, P2^1, P2^2] at 0");
  const L1 = mathx.legendre(1, 0.5);
  assertClose(L1[0], 0.5, 1e-12, "legendre(1,0.5)[0] = 0.5");
  assertClose(Math.abs(L1[1]), Math.sqrt(0.75), 1e-12, "legendre(1,0.5)[1] magnitude sqrt(3)/2"); // DOC-TENSION: Condon-Shortley sign not pinned by doc; magnitude only
  // legendreP: "The single value P_n^m(x); degree capped at 150"
  assertClose(mathx.legendreP(2, 0, 0.5), -0.125, 1e-12, "P2(0.5) = (3x^2-1)/2");
  assertClose(mathx.legendreP(1, 0, 0.7), 0.7, 1e-15, "P1(x) = x");
  // legendreP follows NR plgndr, whose recurrence is valid on |x| <= 1; the doc pins only
// a degree cap, so out-of-domain rows are not derivable (engine returns NaN for |x| > 1).
assertClose(mathx.legendreP(0, 0, 0.7), 1, 1e-15, "P0(x) = 1");
  assertClose(mathx.legendreP(2, 2, 0.5), 2.25, 1e-12, "P_2^2(0.5) = 3(1-x^2)"); // even m: no sign ambiguity
  assertClose(Math.abs(mathx.legendreP(1, 1, 0.5)), Math.sqrt(0.75), 1e-12, "P_1^1 magnitude"); // DOC-TENSION: sign convention not pinned
  assertClose(mathx.legendreP(2, 0, 1), 1, 1e-12, "P_n(1) = 1");
  // airy: "All four Airy values from one evaluation" (standard Ai/Ai'/Bi/Bi' at 0)
  const a0 = mathx.airy(0);
  assertClose(a0.ai, 0.3550280538878172, 1e-9, "Ai(0)");
  assertClose(a0.aip, -0.2588194037928068, 1e-9, "Ai'(0)");
  assertClose(a0.bi, 0.6149266274460007, 1e-9, "Bi(0)");
  assertClose(a0.bip, 0.4482883573538264, 1e-9, "Bi'(0)");
}

// ============================ isPrime / factor / primes (d.ts L3215-3220) ============================
{
  const cases = [
    ["isPrime(2)", 2, true], ["isPrime(3)", 3, true], ["isPrime(4)", 4, false],
    ["isPrime(17)", 17, true], ["isPrime(25)", 25, false],
    ["isPrime(0)", 0, false], ["isPrime(1)", 1, false],
    // isPrime takes a NUMBER: anything past 2^53 rounds before the call (2^61-1 -> 2^61, even),
// so only <= 2^53 inputs are derivable. 2^53-111 = 9007199254740881 is prime and exact.
["isPrime(2^53-111 prime, exact)", 9007199254740881, true],
    // 2^63-1 as a double rounds up to 2^63, which overflows the int64 input -> RangeError, so
// that row is not derivable either. 2^53-1 = 6361*69431*20394401 is exact and composite.
["isPrime(2^53-1 composite, exact)", 9007199254740991, false],
  ];
  for (const [label, x, expected] of cases) assertEq(mathx.isPrime(x), expected, label);
  // factor: "Ascending prime factors with multiplicity; factor(1) is []"
  const cases2 = [
    ["factor(12)", 12, [2, 2, 3]], ["factor(1)", 1, []],
    ["factor(2)", 2, [2]], ["factor(97)", 97, [97]],
    ["factor(360)", 360, [2, 2, 2, 3, 3, 5]],
  ];
  for (const [label, x, expected] of cases2) assert(eqArr(mathx.factor(x), expected), label);
  // primes: "Every prime <= n by sieve, up to 5e7"
  assert(eqArr(mathx.primes(10), [2, 3, 5, 7]), "primes(10)");
  assert(eqArr(mathx.primes(2), [2]), "primes(2)");
  assert(eqArr(mathx.primes(1), []), "primes(1) empty");
  const p31 = mathx.primes(31);
  assertEq(p31.length, 11, "primes(31) has 11 primes <= 31");
  assertEq(p31[10], 31, "primes(31) last is 31");
  assertThrows(() => mathx.primes(50000001), "primes above the documented 5e7 cap is refused");
}

// ============================ gcd / lcm / factorial / abs / bitLen / popcount (d.ts L3222-3236) ============================
{
  const cases = [
    ["gcd(12,18)", 12, 18, 6n], ["gcd(0,5)", 0, 5, 5n], ["gcd(7,13)", 7, 13, 1n],
    ["gcd(12n,18n)", 12n, 18n, 6n], // bigint door parity
    ["lcm(4,6)", 4, 6, 12n], ["lcm(21,6)", 21, 6, 42n],
  ];
  for (const [label, a, b, expected] of cases) assertEq(label.startsWith("gcd") ? mathx.gcd(a, b) : mathx.lcm(a, b), expected, label);
  assertEq(mathx.gcd(12n, 18n), mathx.gcd(12, 18), "gcd number/bigint doors parity");
  assertThrows(() => mathx.gcd(18446744073709551616n, 1n), "gcd refuses BigInt magnitude >= 2^64"); // "wider BigInts are refused"
  assertThrows(() => mathx.lcm(1n, 18446744073709551616n), "lcm refuses BigInt magnitude >= 2^64");
  // factorial: "n! exactly, capped at 10000"
  const cases2 = [
    ["factorial(0)", 0, 1n], ["factorial(1)", 1, 1n], ["factorial(5)", 5, 120n],
    ["factorial(10)", 10, 3628800n], ["factorial(20)", 20, 2432902008176640000n],
  ];
  for (const [label, x, expected] of cases2) assertEq(mathx.factorial(x), expected, label);
  assert(typeof mathx.factorial(10000) === "bigint", "factorial(10000) at the documented cap is accepted");
  assertThrows(() => mathx.factorial(10001), "factorial above the documented 10000 cap is refused");
  // abs/bitLen/popcount: magnitude-based, refusal >= 2^64
  assertEq(mathx.abs(-5n), 5n, "abs(-5n) = 5n");
  assertEq(mathx.abs(5n), 5n, "abs(5n) = 5n");
  const bl = [
    ["bitLen(0n)", 0n, 0], ["bitLen(1n)", 1n, 1], ["bitLen(255n)", 255n, 8],
    ["bitLen(256n)", 256n, 9], ["bitLen(-255n)", -255n, 8], // "the magnitude"
  ];
  for (const [label, x, expected] of bl) assertEq(mathx.bitLen(x), expected, label);
  const pc = [
    ["popcount(0n)", 0n, 0], ["popcount(255n)", 255n, 8], ["popcount(0b1011n)", 0b1011n, 3],
  ];
  for (const [label, x, expected] of pc) assertEq(mathx.popcount(x), expected, label);
  assertThrows(() => mathx.bitLen(18446744073709551616n), "bitLen refuses >= 2^64");
  assertThrows(() => mathx.popcount(18446744073709551616n), "popcount refuses >= 2^64");
}

// ============================ nchoosek / perms / rat (d.ts L3238-3243) ============================
{
  const cases = [
    ["nchoosek(5,2)", 5, 2, 10], ["nchoosek(10,0)", 10, 0, 1],
    ["nchoosek(10,10)", 10, 10, 1], ["nchoosek(52,5)", 52, 5, 2598960],
    ["nchoosek(0,0)", 0, 0, 1],
  ];
  for (const [label, a, b, expected] of cases) assertEq(mathx.nchoosek(a, b), expected, label);
  // perms: "Every permutation, reverse lexicographic, at most 8 elements"
  assert(eqArr2(mathx.perms([1, 2]), [[2, 1], [1, 2]]), "perms([1,2]) reverse lexicographic");
  assertEq(mathx.perms([1, 2, 3]).length, 6, "perms([1,2,3]) has 3! rows");
  assert(eqArr(mathx.perms([1, 2, 3])[0], [3, 2, 1]), "perms first row is reverse-lex largest [3,2,1]");
  assertEq(mathx.perms([1, 2, 3, 4, 5, 6, 7, 8]).length, 40320, "perms of 8 elements = 8! rows (at most 8 accepted)");
  assertThrows(() => mathx.perms([1, 2, 3, 4, 5, 6, 7, 8, 9]), "perms of 9 elements refused (documented 8-element cap)");
  // rat: "Rational approximation by continued fractions within relative tolerance"
  const cases2 = [
    ["rat(0.5)", 0.5, [1, 2]], ["rat(2)", 2, [2, 1]],
    ["rat(0.25)", 0.25, [1, 4]], ["rat(1/3)", 1 / 3, [1, 3]],
  ];
  for (const [label, x, expected] of cases2) assert(eqArr2([mathx.rat(x)], [expected]), label);
}

// ============================ linspace / logspace / cumsum / cumprod / diff (d.ts L3245-3254) ============================
{
  // "n points inclusive of both ends; the last point is exactly b"
  assert(eqArr(mathx.linspace(0, 1, 5), [0, 0.25, 0.5, 0.75, 1]), "linspace(0,1,5) inclusive grid");
  assert(eqArr(mathx.linspace(1, 5, 5), [1, 2, 3, 4, 5]), "linspace(1,5,5) integer grid");
  assert(eqArr(mathx.linspace(0, 3, 2), [0, 3]), "linspace n=2 endpoints only");
  assert(eqArr(mathx.linspace(3, 3, 4), [3, 3, 3, 3]), "linspace degenerate a===b");
  assertEq(mathx.linspace(0.1, 0.2, 3)[2], 0.2, "linspace last point is exactly b");
  // logspace: "10^t over the linspace grid"
  assert(eqArr(mathx.logspace(0, 3, 4), [1, 10, 100, 1000]), "logspace(0,3,4)");
  assert(eqArr(mathx.logspace(-1, 1, 3), [0.1, 1, 10]), "logspace(-1,1,3)");
  // cumsum/cumprod/diff
  assert(eqArr(mathx.cumsum([1, 2, 3]), [1, 3, 6]), "cumsum([1,2,3])");
  assert(eqArr(mathx.cumsum([]), []), "cumsum([]) empty");
  assert(eqArr(mathx.cumprod([1, 2, 3, 4]), [1, 2, 6, 24]), "cumprod([1,2,3,4])");
  assert(eqArr(mathx.cumprod([]), []), "cumprod([]) empty");
  assert(eqArr(mathx.diff([1, 4, 9]), [3, 5]), "diff([1,4,9]) adjacent differences");
  assert(eqArr(mathx.diff([5]), []), "diff single element -> empty");
  assert(eqArr(mathx.diff([]), []), "diff empty -> empty");
}

// ============================ bits namespace (d.ts L3256-3305) ============================
// "Width-parameterised fixed-width bit primitives."
{
  assert(typeof mathx.bits.uintSize === "number", "bits.uintSize is a number"); // value not pinned by doc
  // leading zeros
  const lz = [
    ["leadingZeros8(0)", 0, 8], ["leadingZeros8(1)", 1, 7], ["leadingZeros8(128)", 128, 0],
    ["leadingZeros16(1)", 1, 15], ["leadingZeros16(0)", 0, 16],
    ["leadingZeros32(1)", 1, 31], ["leadingZeros32(0)", 0, 32],
    ["leadingZeros64(1n)", 1n, 63], ["leadingZeros64(0n)", 0n, 64],
  ];
  for (const [label, x, expected] of lz) assertEq(label.includes("8(") ? mathx.bits.leadingZeros8(x) : label.includes("16(") ? mathx.bits.leadingZeros16(x) : label.includes("32(") ? mathx.bits.leadingZeros32(x) : mathx.bits.leadingZeros64(x), expected, label);
  // trailing zeros
  const tz = [
    ["trailingZeros8(2)", 2, 1], ["trailingZeros8(0)", 0, 8], ["trailingZeros8(0b100)", 0b100, 2],
    ["trailingZeros16(2)", 2, 1],
    ["trailingZeros32(8)", 8, 3], ["trailingZeros32(0)", 0, 32],
    ["trailingZeros64(8n)", 8n, 3], ["trailingZeros64(0n)", 0n, 64],
  ];
  for (const [label, x, expected] of tz) assertEq(label.includes("8(") ? mathx.bits.trailingZeros8(x) : label.includes("16(") ? mathx.bits.trailingZeros16(x) : label.includes("32(") ? mathx.bits.trailingZeros32(x) : mathx.bits.trailingZeros64(x), expected, label);
  // population counts
  const oc = [
    ["onesCount8(255)", 255, 8], ["onesCount8(0)", 0, 0], ["onesCount8(0b1011)", 0b1011, 3],
    ["onesCount16(0xFFFF)", 0xFFFF, 16],
    ["onesCount32(0xFFFFFFFF)", 0xFFFFFFFF, 32],
    ["onesCount64(2^64-1)", 18446744073709551615n, 64], ["onesCount64(0b101n)", 0b101n, 2],
  ];
  for (const [label, x, expected] of oc) assertEq(label.includes("8(") ? mathx.bits.onesCount8(x) : label.includes("16(") ? mathx.bits.onesCount16(x) : label.includes("32(") ? mathx.bits.onesCount32(x) : mathx.bits.onesCount64(x), expected, label);
  // bit lengths
  const len = [
    ["len8(0)", 0, 0], ["len8(1)", 1, 1], ["len8(255)", 255, 8],
    ["len16(0xFFFF)", 0xFFFF, 16],
    ["len32(0xFFFFFFFF)", 0xFFFFFFFF, 32],
    ["len64(255n)", 255n, 8],
  ];
  for (const [label, x, expected] of len) assertEq(label.includes("8(") ? mathx.bits.len8(x) : label.includes("16(") ? mathx.bits.len16(x) : label.includes("32(") ? mathx.bits.len32(x) : mathx.bits.len64(x), expected, label);
  // bit reversal
  const rev = [
    ["reverse8(0b11)", 0b11, 192], ["reverse8(0b10000000)", 0b10000000, 1],
    ["reverse8(0)", 0, 0], ["reverse8(255)", 255, 255],
    ["reverse16(1)", 1, 32768],
    ["reverse32(1)", 1, 2147483648],
    ["reverse64(1n)", 1n, 9223372036854775808n],
  ];
  for (const [label, x, expected] of rev) assertEq(label.includes("8(") ? mathx.bits.reverse8(x) : label.includes("16(") ? mathx.bits.reverse16(x) : label.includes("32(") ? mathx.bits.reverse32(x) : mathx.bits.reverse64(x), expected, label);
  // byte reversal
  const rb = [
    ["reverseBytes16(0x00FF)", 0x00FF, 0xFF00],
    ["reverseBytes16(0x1234)", 0x1234, 0x3412],
    ["reverseBytes32(0x000000FF)", 0x000000FF, 0xFF000000],
    ["reverseBytes32(0x12345678)", 0x12345678, 0x78563412],
    ["reverseBytes64(0xFFn)", 0xFFn, 0xFF00000000000000n],
  ];
  for (const [label, x, expected] of rb) assertEq(label.includes("16(") ? mathx.bits.reverseBytes16(x) : label.includes("32(") ? mathx.bits.reverseBytes32(x) : mathx.bits.reverseBytes64(x), expected, label);
  // rotations; "k reduces modulo the width"
  const rotl = [
    ["rotateLeft8(0x80,1)", 0x80, 1, 1],
    ["rotateLeft8(1,8)", 1, 8, 1],
    ["rotateLeft8(0b1100,2)", 0b1100, 2, 48],
    ["rotateLeft16(1,16)", 1, 16, 1],
    ["rotateLeft32(1,31)", 1, 31, 2147483648],
    ["rotateLeft64(1n,64)", 1n, 64, 1n],
  ];
  for (const [label, x, k, expected] of rotl) assertEq(label.includes("8(") ? mathx.bits.rotateLeft8(x, k) : label.includes("16(") ? mathx.bits.rotateLeft16(x, k) : label.includes("32(") ? mathx.bits.rotateLeft32(x, k) : mathx.bits.rotateLeft64(x, k), expected, label);
  const rotr = [
    ["rotateRight8(1,1)", 1, 1, 128],
    ["rotateRight32(1,32)", 1, 32, 1],
  ];
  for (const [label, x, k, expected] of rotr) assertEq(label.includes("8(") ? mathx.bits.rotateRight8(x, k) : mathx.bits.rotateRight32(x, k), expected, label);
  assertEq(mathx.bits.rotateRight8(1, 1), mathx.bits.rotateLeft8(1, 7), "rotateRight is the named twin of rotateLeft (parity)");
  // widening add/sub with carry/borrow in and out — API.md pins the pair order:
  // "bits.add32(a, b, carry) -> [sum, carryOut]" (example: [0, 1]); sub likewise value-first.
  assert(eqArr2([mathx.bits.add32(0xFFFFFFFF, 1, 0)], [[0, 1]]), "add32(0xFFFFFFFF,1,0) -> sum 0, carryOut 1");
  assert(eqArr2([mathx.bits.add32(0xFFFFFFFF, 1, 1)], [[1, 1]]), "add32(0xFFFFFFFF,1,1) -> sum 1, carryOut 1");
  assert(eqArr2([mathx.bits.add32(1, 2, 0)], [[3, 0]]), "add32(1,2,0)");
  assert(eqArr2([mathx.bits.sub32(5, 3, 0)], [[2, 0]]), "sub32(5,3,0)");
  assert(eqArr2([mathx.bits.sub32(0, 1, 0)], [[4294967295, 1]]), "sub32(0,1,0) borrows");
  assert(eqArr2([mathx.bits.add64(18446744073709551615n, 1n, 0n)], [[0n, 1n]]), "add64(2^64-1,1,0)");
  assert(eqArr2([mathx.bits.sub64(0n, 1n, 0n)], [[18446744073709551615n, 1n]]), "sub64(0,1,0) borrows");
  // full-width products: "Full-width product, high word first"
  assert(eqArr2([mathx.bits.mul32(3, 4)], [[0, 12]]), "mul32(3,4)");
  assert(eqArr2([mathx.bits.mul32(0x10000, 0x10000)], [[1, 0]]), "mul32(2^16,2^16) = 2^32 -> hi 1 lo 0");
  assert(eqArr2([mathx.bits.mul64(4294967296n, 4294967296n)], [[1n, 0n]]), "mul64(2^32,2^32)");
  // double-width division: "Divides the double-width hi:lo by y; throws on y==0 or y<=hi"
  // API.md: "bits.div32(hi, lo, y) -> [quo, rem]" (example div32(0,10,3) -> [3,1])
  assert(eqArr2([mathx.bits.div32(0, 12, 3)], [[4, 0]]), "div32(0,12,3)");
  assert(eqArr2([mathx.bits.div32(1, 0, 2)], [[2147483648, 0]]), "div32(1,0,2) = 2^32/2");
  assertThrows(() => mathx.bits.div32(0, 0, 0), "div32 throws on y == 0");
  assertThrows(() => mathx.bits.div32(4, 0, 4), "div32 throws on y <= hi");
  assert(eqArr2([mathx.bits.div64(0n, 12n, 3n)], [[4n, 0n]]), "div64(0,12,3)"); // [quo, rem] per API.md
  assertThrows(() => mathx.bits.div64(0n, 0n, 0n), "div64 throws on y == 0");
  assertEq(mathx.bits.rem32(0, 12, 5), 2, "rem32(0,12,5) = 2");
  assertEq(mathx.bits.rem64(0n, 12n, 5n), 2n, "rem64(0,12,5) = 2n");
}

// ============================ stats namespace (d.ts L3307-3350) ============================
// Contracts that differ from the neighbours, on purpose: see the namespace block comment.
{
  // sum: "Neumaier-compensated; empty array sums to +0"; overflow note: "a partial sum that overflows to Infinity is reported as NaN"
  const sumCases = [
    ["sum([1,2,3])", [1, 2, 3], 6],
    ["sum([]) is +0", [], 0],
    ["sum compensated [1e100,1,-1e100,1]", [1e100, 1, -1e100, 1], 2], // Neumaier recovers the small terms exactly
    ["sum overflow -> NaN", [1e308, 1e308], NaN],
  ];
  for (const [label, input, expected] of sumCases) {
    const got = mathx.stats.sum(input);
    if (typeof expected === "number" && Number.isNaN(expected)) assert(Number.isNaN(got), label);
    else assertEq(got, expected, label);
  }
  // mean: "sum / n; the empty array is 0/0 = NaN"
  const meanCases = [
    ["mean([1,2,3,4])", [1, 2, 3, 4], 2.5],
    ["mean([])", [], NaN],
    ["mean with NaN", [1, NaN, 3], NaN],
  ];
  for (const [label, input, expected] of meanCases) {
    const got = mathx.stats.mean(input);
    if (Number.isNaN(expected)) assert(Number.isNaN(got), label);
    else assertEq(got, expected, label);
  }
  // variance/stddev: "Two-pass variance, sample (n-1) by default; opts.pop divides by n"
  assertClose(mathx.stats.variance([1, 2, 3, 4]), 5 / 3, 1e-15, "variance sample of [1,2,3,4] = 5/3");
  assertClose(mathx.stats.variance([1, 2, 3, 4], { pop: true }), 1.25, 1e-15, "variance pop of [1,2,3,4] = 5/4");
  assertEq(mathx.stats.variance([5, 5, 5]), 0, "variance of constant is 0");
  assertClose(mathx.stats.stddev([1, 2, 3, 4]), Math.sqrt(5 / 3), 1e-12, "stddev sample of [1,2,3,4]");
  assertEq(mathx.stats.stddev([5, 5, 5]), 0, "stddev of constant is 0");
  // median: "quantile(a, 0.5); empty throws RangeError"; NaN element answers NaN
  assertEq(mathx.stats.median([1, 3, 2]), 2, "median odd count");
  assertEq(mathx.stats.median([1, 2, 3, 4]), 2.5, "median even count (R-7 midpoint)");
  assert(Number.isNaN(mathx.stats.median([1, NaN, 3])), "median([1,NaN,3]) is NaN (documented)"); // documented verbatim
  assertThrows(() => mathx.stats.median([]), "median empty throws RangeError", RangeError);
  // quantile: "R-7 linear; q in [0,1] and non-empty (RangeError); NaN element answers NaN"
  const qCases = [
    ["quantile([1,2,3,4],0)", [1, 2, 3, 4], 0, 1],
    ["quantile([1,2,3,4],1)", [1, 2, 3, 4], 1, 4],
    ["quantile([1,2,3,4],0.5)", [1, 2, 3, 4], 0.5, 2.5],
    ["quantile([1,2,3,4],0.25)", [1, 2, 3, 4], 0.25, 1.75], // pos = 0.75 -> 1 + 0.75
  ];
  for (const [label, input, q, expected] of qCases) assertEq(mathx.stats.quantile(input, q), expected, label);
  assertThrows(() => mathx.stats.quantile([], 0.5), "quantile empty throws RangeError", RangeError);
  assertThrows(() => mathx.stats.quantile([1, 2], 1.5), "quantile q outside [0,1] throws RangeError", RangeError);
  // min/max: "Math.min/max semantics: NaN poisons, empty is +Inf/-Inf, -0 wins min and +0 wins max"
  const mm = [
    ["min([3,1,2])", mathx.stats.min([3, 1, 2]), 1],
    ["max([3,1,2])", mathx.stats.max([3, 1, 2]), 3],
    ["min([]) is +Inf", mathx.stats.min([]), Infinity],
    ["max([]) is -Inf", mathx.stats.max([]), -Infinity],
  ];
  for (const [label, got, expected] of mm) assertEq(got, expected, label);
  assert(Number.isNaN(mathx.stats.min([1, NaN, 2])), "min NaN poisons");
  assert(Number.isNaN(mathx.stats.max([1, NaN, 2])), "max NaN poisons");
  assertEq(mathx.stats.min([-0, 0]), -0, "min: -0 wins");
  assertEq(mathx.stats.max([-0, 0]), 0, "max: +0 wins");
  // cov: "Sample covariance (n-1) default; opts.pop divides by n; length mismatch throws RangeError"
  assertClose(mathx.stats.cov([1, 2, 3], [2, 4, 6]), 2, 1e-15, "cov sample of perfectly linear pair = 2");
  assertClose(mathx.stats.cov([1, 2, 3], [2, 4, 6], { pop: true }), 4 / 3, 1e-15, "cov pop = 4/3");
  assertThrows(() => mathx.stats.cov([1, 2], [1, 2, 3]), "cov length mismatch throws RangeError", RangeError);
  // corr: "Pearson r, clamped to [-1,1]; length mismatch or zero variance throws RangeError; NaN inputs answer NaN"
  assertClose(mathx.stats.corr([1, 2, 3], [2, 4, 6]), 1, 1e-12, "corr perfect positive");
  assertClose(mathx.stats.corr([1, 2, 3], [-1, -2, -3]), -1, 1e-12, "corr perfect negative");
  assert(Number.isNaN(mathx.stats.corr([1, NaN, 3], [1, 2, 3])), "corr NaN inputs answer NaN");
  assertThrows(() => mathx.stats.corr([1, 1, 1], [1, 2, 3]), "corr zero variance throws RangeError", RangeError);
  assertThrows(() => mathx.stats.corr([1, 2], [1, 2, 3]), "corr length mismatch throws RangeError", RangeError);
}

// ============================ Expression (d.ts L3352-3359) ============================
// "Compiles an arithmetic string to an RPN program; no eval, no scope."
{
  const cases = [
    ["eval 2+3*4 precedence", "2+3*4", undefined, 14],
    ["eval (2+3)*4 parens", "(2+3)*4", undefined, 20],
    ["eval 10/4", "10/4", undefined, 2.5],
    ["eval with vars a*2+b", "a*2+b", { a: 3, b: 4 }, 10],
  ];
  for (const [label, text, vars, expected] of cases) {
    const ex = new mathx.Expression(text);
    assertEq(ex.eval(vars), expected, label);
  }
  // variables(): "Free variables in first-use order."
  assert(eqArr(new mathx.Expression("a+b*2").variables(), ["a", "b"]), "variables first-use order a,b");
  assert(eqArr(new mathx.Expression("x*x-3*y").variables(), ["x", "y"]), "variables first-use order x,y");
  assert(eqArr(new mathx.Expression("2+3").variables(), []), "variables of constant program is empty");
  // determinism parity: same program, same answer twice
  const ex2 = new mathx.Expression("2+3*4");
  assertEq(ex2.eval(), ex2.eval(), "Expression eval is deterministic");
  assertThrows(() => new mathx.Expression("2++*"), "malformed arithmetic string is refused at compile time");
}

print("bb_mathx: all tests passed (" + n + " assertions)");
