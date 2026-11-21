// covers: dyna:mathx.Expression
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_mathx_expression_tierb");
import * as m from "dyna:mathx";

let n = 0;
function assert(c, msg) { n++; if (!c) throw new Error("assertion failed: " + msg); }
function near(a, b, tol, msg) {
    n++;
    const scale = Math.max(1, Math.abs(b));
    if (!(Math.abs(a - b) <= tol * scale))
        throw new Error("assertion failed: " + msg + " (" + a + " vs " + b + ")");
}
function throws(fn, msg) {
    n++;
    let caught = null;
    try { fn(); } catch (e) { caught = e; }
    if (caught === null) throw new Error("assertion failed: " + msg + " (expected throw)");
}

__kitS.section("mod(1e17,3) is the true remainder 1", () => {
    assert(m.mod(1e17, 3) === 1, "mod(1e17,3) is the true remainder 1");
    assert(m.mod(1e17, 7) === 5, "mod(1e17,7) is 5, never negative for y > 0");
    assert(m.mod(1234567890123456800, 9) === 6, "mod stays exact at 1.2e18");
    assert(m.mod(6.830384611083916e18, 12345) === 7918, "mod at 6.8e18 matches BigInt truth");

    for (const x of [25, 25.5, 26, 26.5]) {
        const exact = Math.exp(x * x) * m.erfc(x);
        near(m.erfcx(x), exact, 1e-14, "erfcx(" + x + ") matches exp(x^2) erfc(x)");
    }
    near(m.erfcx(30), 0.018795888861416745, 1e-15, "erfcx(30) hp reference");

    assert(m.eps(m.realmax()) === 2 ** 971, "eps(realmax) is 2^971 (MATLAB)");
    assert(m.eps(1) === 2.220446049250313e-16, "eps(1) is 2^-52");
    assert(m.eps(4) === 8.881784197001252e-16, "eps(4) is 2^-50");
    assert(m.eps(m.realmin()) === 5e-324, "eps(realmin) is 2^-1074");

    near(m.nthroot(1e300, 3), 1e100, 1e-15, "nthroot(1e300,3) is exact cbrt");
    assert(m.nthroot(-27, 3) === -3, "nthroot(-27,3) = -3");
    assert(m.nthroot(-8, 3) === -2, "nthroot(-8,3) = -2");

    throws(() => m.besselh(1, 1, 2.5), "besselh kind 2.5 is refused");
    assert(m.besselh(0, 1)[1] === m.bessely(0, 1), "besselh defaults to kind 1");

    throws(() => m.isPrime(2n ** 64n + 1n), "isPrime refuses BigInts above uint64");
    throws(() => m.gcd(2n ** 64n, 2n), "gcd refuses BigInts above int64");
    throws(() => m.lcm(-(2n ** 63n) - 1n, 2n), "lcm refuses BigInts below int64");
    assert(m.isPrime(9223372036854775807n) === false, "2^63-1 = 7*73*127*337*92737*649657 is composite");

    throws(() => m.bitLen(m.factorial(21)), "bitLen refuses >64-bit magnitudes");
    throws(() => m.popcount(-(2n ** 200n)), "popcount refuses >64-bit magnitudes");
    throws(() => m.abs(-(2n ** 200n)), "abs refuses >64-bit magnitudes");
    assert(m.bitLen(255n) === 8, "bitLen(255n) is 8");
    assert(m.bitLen(-(2n ** 63n)) === 64, "bitLen(-2^63) is 64");
    assert(m.bitLen(0n) === 0, "bitLen(0n) is 0");
    assert(m.popcount(255n) === 8, "popcount(255n) is 8");
    assert(m.abs(-9007199254740993n) === 9007199254740993n, "abs is the magnitude");
    assert(m.abs(2n ** 63n - 1n) === 2n ** 63n - 1n, "abs keeps uint64 magnitudes");

    for (const c of [341, 561, 645, 1105, 4141, 3215031751])
        assert(m.isPrime(c) === false, "isPrime(" + c + ") is false (pseudoprime)");
    assert(m.isPrime(2305843009213693951n) === true, "2^61-1 is prime (Mersenne)");

    assert(m.factorial(10000).toString().length === 35660, "10000! has 35660 digits");

    {
        const e = new m.Expression("a*x^2 + b");
        assert(JSON.stringify(e.variables()) === '["a","x","b"]', "variables in first-use order");
        const proto = Object.create({ a: 3 });
        let refused = false;
        try { e.eval(proto); } catch (err) { refused = true; }
        assert(refused, "a prototype-chain variable is NOT readable");
        throws(() => e.eval(new Proxy({}, {
            get() { throw new Error("getter ran"); },
            has() { return true; },
        })), "eval reads own properties, never traps");
        assert(new m.Expression("-x^2").eval({ x: 3 }) === -9,
            "unary minus binds below ^ (MATLAB -2^2 = -4)");
        assert(new m.Expression("2^-2").eval() === 0.25, "2^-2 = 0.25");
        assert(new m.Expression("1/0").eval() === Infinity, "division is IEEE, not a throw");
        assert(new m.Expression("sqrt(pi)").eval() === Math.sqrt(Math.PI), "pi is a constant");
        throws(() => new m.Expression("("), "unmatched ( is a SyntaxError");
        throws(() => new m.Expression(""), "the empty string is a SyntaxError");
        throws(() => new m.Expression("1+"), "a trailing operator is a SyntaxError");
        throws(() => new m.Expression("sin()"), "an empty call is a SyntaxError");
        throws(() => new m.Expression("f(1)"), "an unknown function is refused");
        throws(() => new m.Expression("x").eval(), "eval needs values for variables");
        throws(() => new m.Expression("x").eval({ x: "3" }), "a non-number value is refused");
        let deep = "1";
        for (let i = 0; i < 300; i++) deep = "(" + deep + "+1)";
        throws(() => new m.Expression(deep), "nesting past the compile stack is refused");
        assert(new m.Expression("(" + "(".repeat(200) + "1" + ")".repeat(200) + ")").eval() === 1,
            "nesting within the compile stack works");
    }

    {
        const rotl = (x, k, w) => {
            const mask = (1n << BigInt(w)) - 1n;
            const s = ((k % w) + w) % w;
            const xb = BigInt(x) & mask;
            return ((xb << BigInt(s)) | (xb >> BigInt(w - s))) & mask;
        };
        const rev = (x, w) => {
            let r = 0n; const xb = BigInt(x);
            for (let i = 0; i < w; i++) r = (r << 1n) | ((xb >> BigInt(i)) & 1n);
            return r;
        };
        const pop = (x) => { let c = 0n; let v = BigInt(x); while (v) { c += v & 1n; v >>= 1n; } return Number(c); };
        for (const [w, vals] of [
            [8, [0, 1, 0x80, 0xff, 0x5a, 200]],
            [16, [0, 1, 0x8000, 0xffff, 0x1234]],
            [32, [0, 1, 2 ** 31, 2 ** 32 - 1, 0xdeadbeef]],
        ]) {
            const W = w === 8 ? "8" : w === 16 ? "16" : "32";
            for (const v of vals) {
                assert(m.bits["onesCount" + W](v) === pop(v), "onesCount" + W + "(" + v + ")");
                assert(m.bits["reverse" + W](v) === Number(rev(v, w)), "reverse" + W + "(" + v + ")");
                let lz = w, t = v;
                while (t) { lz--; t >>>= 1; }
                assert(m.bits["leadingZeros" + W](v) === lz, "leadingZeros" + W + "(" + v + ")");
                for (const k of [-1, 0, 1, 5, w, w + 3])
                    assert(m.bits["rotateLeft" + W](v, k) === Number(rotl(v, k, w)),
                        "rotateLeft" + W + "(" + v + "," + k + ")");
            }
            assert(m.bits["leadingZeros" + W](0) === w, "leadingZeros" + W + "(0) is the width");
            assert(m.bits["trailingZeros" + W](0) === w, "trailingZeros" + W + "(0) is the width");
        }
        for (const [a, b] of [[0xffffffff, 0xffffffff], [0x12345678, 0x9abcdef0], [1, 1], [0, 7]]) {
            const [hi, lo] = m.bits.mul32(a, b);
            assert(BigInt(hi) * 4294967296n + BigInt(lo) === BigInt(a) * BigInt(b), "mul32(" + a + "," + b + ")");
        }
        for (const [a, b] of [[2n ** 64n - 1n, 2n ** 64n - 1n], [12345678901234567890n, 9876543210987654321n]]) {
            const [hi, lo] = m.bits.mul64(a, b);
            assert(((BigInt(hi) << 64n) | BigInt(lo)) === BigInt.asUintN(128, a * b), "mul64 hi:lo");
        }
        throws(() => m.bits.div32(1, 0, 0), "div32 by zero throws");
        throws(() => m.bits.div32(5, 1, 5), "div32 throws when y <= hi");
        const qr = m.bits.div32(0, 100, 7);
        assert(qr[0] === 14 && qr[1] === 2, "div32(0,100,7) = [14,2]");
        assert(m.bits.rem32(5, 100, 7) === 1, "rem32(5,100,7) = (5*2^32+100)%7, no overflow panic");
    }

    for (const p of [0.01, 0.5, 0.9, 0.999]) {
        for (const a of [0.5, 2, 10.5]) {
            const x = m.gammaincinv(p, a);
            near(m.gammainc(x, a), p, 1e-12, "gammaincinv(" + p + "," + a + ") roundtrips");
        }
    }
    near(m.betainc(0.2, 8, 3), 7.3728e-5 + 4.096e-6 + 1.024e-7, 1e-14,
        "betainc matches the binomial closed form");

    for (let jn = 1; jn <= 6; jn++) {
        const lhs = m.besselj(jn - 1, 5) + m.besselj(jn + 1, 5);
        const rhs = (2 * jn / 5) * m.besselj(jn, 5);
        near(lhs, rhs, 1e-13, "J recurrence at n=" + jn);
    }
    const zx = 2.5;
    near(m.besseli(0.5, zx), Math.sqrt(2 / (Math.PI * zx)) * Math.sinh(zx), 1e-15, "I_{1/2} closed form");
    near(m.besselk(0.5, zx), Math.sqrt(Math.PI / (2 * zx)) * Math.exp(-zx), 1e-15, "K_{1/2} closed form");
    let ww = 0;
    for (let ax = -15; ax <= 15; ax += 0.5) {
        const a = m.airy(ax);
        const err = Math.abs(a.ai * a.bip - a.aip * a.bi - 1 / Math.PI);
        if (err > ww) ww = err;
    }
    assert(ww < 5e-12, "Airy Wronskian holds across regimes (worst " + ww + ")");
});
__kitS.end();

print("test_mathx_tierb: all " + n + " assertions passed");
