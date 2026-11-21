// covers: dyna:mathx.bits
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_mathx_bits_matlab");
import { mod, rem, fmod, fix, sign, nthroot, nextpow2, pow2, deg2rad, rad2deg, eps, realmin, realmax, flintmax, beta, betaln, gammaln, psi, erfinv, erfcinv, erfcx, expint, factor, primes, nchoosek, rat, linspace, logspace, cumsum, cumprod, diff, erf, erfc, lgamma, trunc, bits } from "dyna:mathx";

let n = 0;
function assert(c, m) { n++; if (!c) throw new Error("assertion failed: " + m); }
function eq(got, want, m) {
    n++;
    if (!Object.is(got, want))
        throw new Error("assertion failed: " + m + "\n  got:  " + got + "\n  want: " + want);
}
function close(got, want, tol, m) {
    n++;
    const err = Math.abs(got - want);
    const rel = want === 0 ? err : err / Math.abs(want);
    if (!(err <= tol || rel <= tol))
        throw new Error("assertion failed: " + m +
            "\n  got:  " + got + "\n  want: " + want +
            "\n  abs err " + err.toExponential(2) + ", rel " + rel.toExponential(2) +
            " (tol " + tol + ")");
}
function eqJSON(got, want, m) {
    n++;
    const a = JSON.stringify(got), b = JSON.stringify(want);
    if (a !== b) throw new Error("assertion failed: " + m + "\n  got:  " + a + "\n  want: " + b);
}
function throwsRange(fn, m) {
    n++;
    try { fn(); } catch (e) {
        if (e instanceof RangeError) return;
        throw new Error("assertion failed (wrong error): " + m + " -> " + e);
    }
    throw new Error("assertion failed (expected RangeError): " + m);
}

__kitS.section("bits is a namespace object", () => {
    assert(typeof bits === "object", "bits is a namespace object");
    eq(bits.uintSize, 64, "bits.uintSize");

    eq(bits.leadingZeros8(1), 7, "leadingZeros8");
    eq(bits.leadingZeros16(1), 15, "leadingZeros16");
    eq(bits.leadingZeros32(1), 31, "leadingZeros32");
    eq(bits.leadingZeros64(1n), 63, "leadingZeros64");
    eq(bits.leadingZeros32(0), 32, "leadingZeros of 0 is the width");
    eq(bits.trailingZeros32(8), 3, "trailingZeros32");
    eq(bits.trailingZeros32(0), 32, "trailingZeros of 0 is the width");
    eq(bits.trailingZeros64(0n), 64, "trailingZeros64(0)");
    eq(bits.onesCount32(255), 8, "onesCount32");
    eq(bits.onesCount64(0xffffn), 16, "onesCount64");
    eq(bits.onesCount8(0xff), 8, "onesCount8");
    eq(bits.len32(1000), 10, "len32");
    eq(bits.len32(0), 0, "len(0) is 0");
    eq(bits.len64(0n), 0, "len64(0)");

    eq(bits.reverse8(19), 200, "reverse8(0b00010011) = 0b11001000");
    eq(bits.reverseBytes32(0x01020304), 0x04030201, "reverseBytes32");
    eq(bits.reverseBytes16(0x0102), 0x0201, "reverseBytes16");
    eq(bits.reverse32(bits.reverse32(0xdeadbeef)), 0xdeadbeef, "reverse32 is an involution");
    eq(bits.reverse64(bits.reverse64(0xdeadbeefcafebaben)), 0xdeadbeefcafebaben,
       "reverse64 is an involution");

    eq(bits.rotateLeft8(1, -1), 128, "rotateLeft8 with negative k rotates right");
    eq(bits.rotateLeft8(1, 1), 2, "rotateLeft8");
    eq(bits.rotateLeft8(1, 8), 1, "a full rotation is the identity");
    eq(bits.rotateLeft8(1, 0), 1, "rotate by 0 is the identity");
    eq(bits.rotateLeft32(0x80000000, 1), 1, "rotateLeft32 wraps the top bit");
    eq(bits.rotateLeft64(1n, 64), 1n, "rotateLeft64 full rotation");

    eqJSON(bits.add32(1, 2, 0), [3, 0], "add32");
    eqJSON(bits.add32(0xffffffff, 1, 0), [0, 1], "add32 carries out");
    eqJSON(bits.add64(0xffffffffffffffffn, 1n, 0n).map(String), ["0", "1"], "add64 carries");
    eqJSON(bits.sub32(0, 1, 0), [0xffffffff, 1], "sub32 borrows");
    eqJSON(bits.mul32(0xffffffff, 2), [1, 0xfffffffe], "mul32 -> [hi, lo]");
    eqJSON(bits.mul64(0xffffffffffffffffn, 0xffffffffffffffffn).map(String),
           ["18446744073709551614", "1"], "mul64 -> [hi, lo]");
    eqJSON(bits.div32(0, 100, 7), [14, 2], "div32");
    eq(bits.rem32(0, 100, 7), 2, "rem32");
    throwsRange(() => bits.div32(0, 1, 0), "div32 by zero");
    throwsRange(() => bits.div32(5, 0, 1), "div32 quotient overflow");
    throwsRange(() => bits.rem32(0, 1, 0), "rem32 by zero");
    eq(bits.rem32(5, 0, 1), 0, "rem32 allows an overflowing quotient");
});

await __kitS.section("dyna:bits should no longer be importable", async () => {
    n++;
    let gone = false;
    try { await import("dyna:bits"); } catch (e) { gone = true; }
    if (!gone) throw new Error("dyna:bits should no longer be importable");
});
__kitS.end();

print("test_mathx_matlab: all " + n + " tests passed");
