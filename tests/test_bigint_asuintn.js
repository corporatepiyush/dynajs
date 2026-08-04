/* test_bigint_asuintn.js -- BigInt.asUintN / asIntN against an INDEPENDENT
 * reference, swept across every width from 0 to 300.
 *
 * The defect this pins: asUintN(bits, a) is `a mod 2^bits`, which is
 * non-negative BY DEFINITION, and two separate paths could return a negative
 * BigInt anyway.
 *
 *   - Returning the operand unchanged whenever `bits` covered its width made
 *     BigInt.asUintN(64, -1n) evaluate to -1n instead of 18446744073709551615n.
 *     137 of the first 200 widths were wrong.
 *   - The truncation path allocated ceil(bits/LIMB) limbs, so a result whose
 *     top bit is set IS negative in two's complement: asUintN(64, 2n**63n)
 *     came back as -2^63.
 *
 * The second one only appeared after the first was fixed, which is the argument
 * for sweeping the whole width range rather than testing the case that failed.
 *
 * THE REFERENCE IS COMPUTED HERE, in JS BigInt arithmetic, from the definition
 * in ECMA-262 21.2.2.1/21.2.2.2 -- never read back from the engine. An expected
 * value recorded from the code under test freezes today's behaviour including
 * its bugs, and six months later is indistinguishable from a real vector.
 *
 * Run: dynajs tests/test_bigint_asuintn.js
 */
"use strict";

let pass = 0, fail = 0;
function ok(c, m) { if (c) pass++; else { fail++; if (fail <= 12) print("FAIL: " + m); } }

/* ECMA-262 21.2.2.1 BigInt.asUintN( bits, bigint ): ℝ(bigint) modulo 2^bits. */
function refUint(bits, a) {
  const m = 1n << BigInt(bits);
  return ((a % m) + m) % m;
}
/* ECMA-262 21.2.2.2 BigInt.asIntN: the same, mapped into [-2^(bits-1), 2^(bits-1)). */
function refInt(bits, a) {
  if (bits === 0) return 0n;
  const m = 1n << BigInt(bits), half = 1n << BigInt(bits - 1);
  const v = ((a % m) + m) % m;
  return v >= half ? v - m : v;
}

/* Values chosen to straddle every representation boundary the implementation
   has: the short/heap split, a limb edge, and the sign bit of a limb. */
const VALUES = [
  0n, 1n, -1n, 2n, -2n, 255n, -255n,
  (1n << 31n) - 1n, 1n << 31n, -(1n << 31n),
  (1n << 32n) - 1n, 1n << 32n, -(1n << 32n),
  (1n << 62n), (1n << 63n) - 1n, 1n << 63n, -(1n << 63n),
  (1n << 64n) - 1n, 1n << 64n, -(1n << 64n), -((1n << 64n) + 1n),
  12345678901234567890n, -12345678901234567890n,
  (1n << 127n) - 1n, -(1n << 127n),
  (1n << 200n) - 1n, -(1n << 200n) - 7n,
];

let negatives = 0, wrongU = 0, wrongI = 0, n = 0;
for (let bits = 0; bits <= 300; bits++) {
  for (const a of VALUES) {
    n++;
    const u = BigInt.asUintN(bits, a);
    if (u !== refUint(bits, a)) {
      wrongU++;
      if (wrongU <= 6) print("  asUintN(" + bits + ", " + a + ") = " + u + " want " + refUint(bits, a));
    }
    /* The invariant on its own, so a wrong-but-non-negative answer and a
       negative one are distinguishable in the output. */
    if (u < 0n) negatives++;
    if (bits > 0 && u >= (1n << BigInt(bits))) negatives++;

    const s = BigInt.asIntN(bits, a);
    if (s !== refInt(bits, a)) {
      wrongI++;
      if (wrongI <= 6) print("  asIntN(" + bits + ", " + a + ") = " + s + " want " + refInt(bits, a));
    }
  }
}

ok(wrongU === 0, "asUintN matches a mod 2^bits over " + n + " cases (" + wrongU + " wrong)");
ok(negatives === 0, "asUintN result is always in [0, 2^bits) (" + negatives + " outside)");
ok(wrongI === 0, "asIntN matches its definition (" + wrongI + " wrong)");

/* Spot values a reader can check by eye, cited to the spec rather than to a
   previous run of this engine. */
ok(BigInt.asUintN(64, -1n) === 18446744073709551615n, "asUintN(64,-1n) is 2^64-1");
ok(BigInt.asUintN(64, 1n << 63n) === 9223372036854775808n, "asUintN(64,2^63) is 2^63");
ok(BigInt.asUintN(0, -1n) === 0n, "asUintN(0, anything) is 0");
ok(BigInt.asUintN(1, -1n) === 1n, "asUintN(1,-1n) is 1");
ok(BigInt.asIntN(64, 18446744073709551615n) === -1n, "asIntN(64,2^64-1) is -1");
ok(BigInt.asIntN(8, 255n) === -1n, "asIntN(8,255n) is -1");
ok(typeof BigInt.asUintN(64, -1n) === "bigint", "the result is a BigInt");

print("test_bigint_asuintn: " + pass + " passed, " + fail + " failed (" + n + " swept cases)");
if (fail) throw new Error(fail + " failures");
