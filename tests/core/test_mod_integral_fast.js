// covers: c:src/vm/interpreter.inc.c
// x % y on integral doubles up to 2^53 takes an int64 remainder instead of fmod. The oracle is BigInt arithmetic
// (truncated division, sign of the dividend), which shares no code with either path; the IEEE edge cases (zero
// divisor, infinities, NaN, -0) are spelled out from the specification's Number::remainder.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_mod_integral_fast");
let failures = 0,
    passed = 0;
function ok(cond, msg) {
    if (cond) passed++;
    else {
        failures++;
        if (failures < 30) print("FAIL " + msg);
    }
}
const ref = (x, y) => {
    const r = Number(BigInt(x) % BigInt(y));
    return r === 0 ? (Object.is(x, -0) || x < 0 ? -0 : 0) : r;
};
let seed = 99;
const rnd = () => ((seed = (Math.imul(seed ^ (seed >>> 15), 0x2c1b3c6d) + 0x6d2b79f5) >>> 0) / 2 ** 32);
const big = [2 ** 31, 2 ** 31 + 1, 2 ** 32 - 1, 2 ** 32, 3e9, 1e15, 2 ** 53 - 1, 2 ** 53, 4503599627370497];
const divs = [1, 2, 3, 7, 10, 524288, 2 ** 31, 2 ** 32 + 5, 1e12 + 3, 2 ** 53, 2 ** 53 - 1];
// every dividend here is at least 2^31, so it is held as a double and reaches the float path
__kitS.section("every dividend here is at least 2^31, so it is held as a double and reaches the float path", () => {
for (const x0 of big)
    for (const y0 of divs)
        for (const [sx, sy] of [
            [1, 1],
            [-1, 1],
            [1, -1],
            [-1, -1],
        ]) {
            const x = sx * x0,
                y = sy * y0;
            const got = x % y,
                want = ref(x, y);
            ok(Object.is(got, want), `${x} % ${y}: got ${got}, want ${want}`);
        }
for (let i = 0; i < 20000; i++) {
    const x = Math.floor((rnd() - 0.5) * 2 ** 54),
        y = Math.floor((rnd() - 0.5) * 2 ** (1 + Math.floor(rnd() * 53))) || 1;
    if (Math.abs(x) > 2 ** 53) continue;
    const got = x % y,
        want = ref(x, y);
    ok(Object.is(got, want), `random ${x} % ${y}: got ${got}, want ${want}`);
}
// exact multiples: the result is a zero carrying the dividend's sign
ok(Object.is(-6e9 % 3e9, -0), "-6e9 % 3e9 is -0");
ok(Object.is(6e9 % -3e9, 0), "6e9 % -3e9 is +0");
ok(Object.is(-0 % 5, -0) && Object.is(-0 % 5e9, -0), "-0 % y is -0");
ok(Object.is(0 % -5e9, 0), "0 % -5e9 is +0");
});
// outside the fast path's domain: the fmod answers
__kitS.section("outside the fast path's domain: the fmod answers", () => {
ok(Number.isNaN(3e9 % 0) && Number.isNaN(3e9 % -0), "x % 0 is NaN");
ok(Number.isNaN(Infinity % 3e9) && Number.isNaN(NaN % 3e9) && Number.isNaN(3e9 % NaN), "Infinity / NaN operands give NaN");
ok(3e9 % Infinity === 3e9 && Object.is(-3e9 % -Infinity, -3e9), "x % +-Infinity is x");
ok(2 ** 60 % 7 === Number(2n ** 60n % 7n), "2^60 % 7 (above 2^53, exact through fmod)");
ok((3e9 + 0.5) % 7 === 3e9 % 7 + 0.5, "a fractional dividend still goes through fmod");
ok(2 ** 53 % 3 === Number(2n ** 53n % 3n) && -(2 ** 53) % 3 === -Number(2n ** 53n % 3n), "the 2^53 boundary itself");
});
// the same rule in the compound and generic paths
__kitS.section("the same rule in the compound and generic paths", () => {
let a = 3e9;
a %= 7;
ok(a === 3e9 % 7 && a === Number(3000000000n % 7n), "%= on a large double");
ok(Object.is(Object(3e9) % 7, Number(3000000000n % 7n)), "a boxed Number operand (generic path)");
print(`mod integral fast: ${passed} passed, ${failures} failed`);
if (failures) throw new Error(`${failures} failures`);
});
__kitS.end();
