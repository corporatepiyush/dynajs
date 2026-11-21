// covers: dyna:mathx.factorial
// factorial and lcm build their BigInt from base-1e9 limbs in C (JS_NewBigUintFromWords32), no longer by evaluating a
// decimal literal per call. Oracles: the BigInt product 1n*2n*...*n for every n up to 1200 plus 1500, 2047, 4096, 9999 and the cap 10000 (every limb count and
// every 32-bit word boundary the conversion crosses), and lcm(a, b) = |a*b| / gcd over operands whose product passes
// 2^64 (the 128-bit path), checked against BigInt arithmetic.
import * as mx from "dyna:mathx";
import { sections } from "../lib/kit.js";
const S = sections("test_mathx_factorial_bigint_words");
let checks = 0, fails = 0;
function eq(got, want, what) { checks++; if (got !== want) { fails++; if (fails < 20) print("FAIL: " + what + ": got " + got + ", want " + want); } }
S.section("factorial matches the BigInt product", () => {
let f = 1n;
const sparse = new Set([1500, 2047, 4096, 9999, 10000]);
for (let n = 0; n <= 10000; n++) {
    if (n > 0) f *= BigInt(n);
    if (n > 1200 && !sparse.has(n)) continue;
    const got = mx.factorial(n);
    eq(got, f, "factorial(" + n + ")");
    eq(typeof got, "bigint", "factorial(" + n + ") type");
}
let threw = false;
try { mx.factorial(10001); } catch (e) { threw = e instanceof RangeError || e instanceof TypeError; }
eq(threw, true, "factorial(10001) throws");
});
function gcdB(a, b) { a = a < 0n ? -a : a; b = b < 0n ? -b : b; while (b) [a, b] = [b, a % b]; return a; }
S.section("lcm past 2^64 matches BigInt arithmetic", () => {
let s = 0x9e3779b97f4a7c15n;
    function rnd() { s ^= s << 13n; s &= (1n << 64n) - 1n; s ^= s >> 7n; s ^= s << 17n; s &= (1n << 64n) - 1n; return s; }
for (let i = 0; i < 4000; i++) {
    let a = BigInt.asIntN(63, rnd()), b = BigInt.asIntN(63, rnd());
    if (i % 3 === 0) a >>= BigInt(i % 50);
    if (a === 0n || b === 0n) continue;
    const want = (a < 0n ? -a : a) / gcdB(a, b) * (b < 0n ? -b : b);
    eq(mx.lcm(a, b), want, "lcm(" + a + ", " + b + ")");
}
eq(mx.lcm(0x7fffffffffffffffn, 0x7ffffffffffffffen), 0x7fffffffffffffffn * 0x7ffffffffffffffen, "lcm of two coprime 63-bit values");
});
S.end();
print("test_mathx_factorial_bigint_words: " + (fails ? fails + " failures of " + checks : "all " + checks + " checks passed"));
if (fails) throw new Error(fails + " failures");
