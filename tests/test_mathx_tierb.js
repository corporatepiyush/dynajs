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

{
    near(m.gammainc(2, 1), 1 - Math.exp(-2), 1e-14, "gammainc(x=2, a=1)");
    near(m.gammainc(1, 2), 1 - 2 * Math.exp(-1), 1e-14, "gammainc(x=1, a=2)");
    assert(m.gammainc(2, 1) !== m.gammainc(1, 2),
        "the two argument orders differ, so the order is actually tested");

    near(m.gammainc(3, 0.5), m.erf(Math.sqrt(3)), 1e-13, "gammainc vs erf");

    near(m.gammainc(2, 1, "upper"), Math.exp(-2), 1e-13, "gammainc upper");
    near(m.gammainc(40, 1, "upper"), Math.exp(-40), 1e-12, "gammainc upper, far tail");
    near(m.gammainc(5, 2) + m.gammainc(5, 2, "upper"), 1, 1e-15, "P + Q = 1");

    near(m.gammainc(2, 1, "lower"), m.gammainc(2, 1), 0, "explicit lower");

    for (const [x, a] of [[3.5, 2], [0.2, 0.7], [80, 60]])
        near(m.gammaincinv(m.gammainc(x, a), a), x, 1e-10,
            "gammaincinv round trip at " + x + "," + a);
}

{
    near(m.betainc(0.5, 2, 3), 0.6875, 1e-14, "betainc(0.5,2,3) = 11/16");
    near(m.betainc(0.3, 1, 1), 0.3, 1e-15, "I_x(1,1) = x");
    near(m.betainc(0.3, 2, 5) + m.betainc(0.7, 5, 2), 1, 1e-14, "betainc symmetry");
    assert(m.betainc(0.5, 2, 3) !== m.betainc(0.5, 3, 2),
        "a and b are not interchangeable, so their order is tested");
    for (const [p, a, b] of [[0.6875, 2, 3], [0.1, 0.5, 4], [0.99, 7, 2]])
        near(m.betainc(m.betaincinv(p, a, b), a, b), p, 1e-10,
            "betaincinv round trip at " + p);
}

{
    for (const x of [0.05, 0.4, 0.6, 3.7, 19.5, 21, 200])
        near(m.besseli(0, x) * m.besselk(1, x) + m.besseli(1, x) * m.besselk(0, x),
            1 / x, 1e-12, "Wronskian at x=" + x);

    assert(Math.abs(m.besseli(-1 / 3, 0.5) - m.besseli(1 / 3, 0.5)) > 1e-3,
        "I_-1/3 and I_1/3 differ at small x");

    for (const k of [1, 2, 3])
        near(m.besselj(k - 1, 4) + m.besselj(k + 1, 4), (2 * k / 4) * m.besselj(k, 4),
            1e-13, "besselj recurrence k=" + k);
    near(m.bessely(0, 3) + m.bessely(2, 3), (2 / 3) * m.bessely(1, 3), 1e-13,
        "bessely recurrence");

    assert(m.besselk(0, 800) === 0, "K underflows past 745, as the double does");
    assert(!isFinite(m.besseli(0, 800)), "I overflows past 713, as the double does");
    near(m.besseliScaled(0, 800) * m.besselkScaled(1, 800) +
         m.besseliScaled(1, 800) * m.besselkScaled(0, 800), 1 / 800, 1e-12,
        "the scaled pair still satisfies the Wronskian where the raw pair cannot");

    const h1 = m.besselh(2, 3.5, 1), h2 = m.besselh(2, 3.5, 2);
    assert(Array.isArray(h1) && h1.length === 2, "besselh returns a pair");
    near(h1[0], m.besselj(2, 3.5), 0, "H1 real part is J");
    near(h1[1], m.bessely(2, 3.5), 0, "H1 imaginary part is Y");
    near(h2[1], -m.bessely(2, 3.5), 0, "H2 conjugates");
    near(m.besselh(2, 3.5)[1], h1[1], 0, "kind defaults to 1");

    throws(() => m.besselj(0.5, 1), "besselj rejects a fractional order");
    throws(() => m.bessely(0.5, 1), "bessely rejects a fractional order");
    throws(() => m.besselh(0.5, 1), "besselh rejects a fractional order");
    throws(() => m.besselh(1, 1, 3), "besselh rejects a kind that is not 1 or 2");
}

{
    const [K0, E0] = m.ellipke(0);
    near(K0, Math.PI / 2, 1e-15, "K(0) = pi/2");
    near(E0, Math.PI / 2, 1e-15, "E(0) = pi/2");
    near(m.ellipke(1)[1], 1, 1e-15, "E(1) = 1");
    assert(!isFinite(m.ellipke(1)[0]), "K(1) diverges");

    for (const mm of [0.1, 0.3, 0.5, 0.9]) {
        const [K, E] = m.ellipke(mm), [Kc, Ec] = m.ellipke(1 - mm);
        near(E * Kc + Ec * K - K * Kc, Math.PI / 2, 1e-14,
            "Legendre relation at m=" + mm);
    }

    for (const mm of [0, 0.25, 0.5, 0.99, 1])
        for (const u of [-3.1, -0.4, 0, 1.3, 5.7]) {
            const { sn, cn, dn } = m.ellipj(u, mm);
            near(sn * sn + cn * cn, 1, 1e-14, "sn^2+cn^2 at " + u + "," + mm);
            near(dn * dn + mm * sn * sn, 1, 1e-14, "dn^2+m sn^2 at " + u + "," + mm);
        }
    near(m.ellipj(1.3, 0).sn, Math.sin(1.3), 1e-14, "m=0 degenerates to sin");
    near(m.ellipj(1.3, 1).sn, Math.tanh(1.3), 1e-14, "m=1 degenerates to tanh");
    throws(() => m.ellipj(1, 2), "ellipj rejects m outside [0,1]");
    throws(() => m.ellipj(1, -0.5), "ellipj rejects negative m");
}

{
    const col = m.legendre(3, 0.4);
    assert(Array.isArray(col) && col.length === 4, "legendre returns the m=0..n column");
    near(col[0], 0.5 * (5 * 0.4 ** 3 - 3 * 0.4), 1e-14, "P_3^0");
    near(col[1], m.legendreP(3, 1, 0.4), 0, "the column agrees with legendreP");
    assert(m.legendre(0, 0.5).length === 1, "degree 0 is a single value");

    near(m.legendreP(1, 0, 0.4), 0.4, 1e-15, "P_1^0 = x");
    near(m.legendreP(2, 0, 0.4), 0.5 * (3 * 0.16 - 1), 1e-15, "P_2^0");
    near(m.legendreP(1, 1, 0.4), -Math.sqrt(1 - 0.16), 1e-15,
        "P_1^1 carries the Condon-Shortley minus");
    near(m.legendreP(2, 2, 0.4), 3 * (1 - 0.16), 1e-14, "P_2^2");

    throws(() => m.legendre(2.5, 0.4), "legendre rejects a fractional degree");
    throws(() => m.legendre(-1, 0.4), "legendre rejects a negative degree");
    assert(Number.isNaN(m.legendreP(2, 3, 0.4)), "order above degree is NaN");
    assert(Number.isNaN(m.legendreP(2, 0, 1.5)), "|x| > 1 is NaN");
}

{
    near(m.polygamma(0, 1), -0.5772156649015329, 1e-14, "psi(1) = -euler");
    near(m.polygamma(0, 2.5), m.psi(2.5), 0,
        "polygamma(0, x) IS psi(x) -- one implementation, not two");
    near(m.polygamma(1, 1), Math.PI ** 2 / 6, 1e-14, "trigamma(1) = zeta(2)");
    near(m.polygamma(1, 0.5), Math.PI ** 2 / 2, 1e-14, "trigamma(1/2)");
    for (const k of [0, 1, 2, 3]) {
        let f = 1;
        for (let i = 2; i <= k; i++) f *= i;
        for (const x of [0.3, 1.7, 9, 30])
            near(m.polygamma(k, x) - m.polygamma(k, x + 1),
                (k % 2 ? 1 : -1) * f / Math.pow(x, k + 1), 1e-12,
                "polygamma recurrence n=" + k + " x=" + x);
    }
    throws(() => m.polygamma(-1, 1), "polygamma rejects a negative order");
    throws(() => m.polygamma(1.5, 1), "polygamma rejects a fractional order");
    assert(Number.isNaN(m.polygamma(1, -2)), "a pole is NaN");
}

{
    for (const x of [-30, -12, -7.5, -7, -6.9, -3, -0.5, 0, 0.05, 0.1, 0.2, 3, 15, 30]) {
        const a = m.airy(x);
        near(a.ai * a.bip - a.aip * a.bi, 1 / Math.PI, 1e-9,
            "Airy Wronskian at x=" + x);
    }
    const a0 = m.airy(0);
    near(a0.ai, Math.pow(3, -2 / 3) / m.gamma(2 / 3), 1e-14, "Ai(0)");
    near(a0.aip, -Math.pow(3, -1 / 3) / m.gamma(1 / 3), 1e-14, "Ai'(0)");
    near(a0.bi, Math.pow(3, -1 / 6) / m.gamma(2 / 3), 1e-14, "Bi(0)");
    near(a0.bip, Math.pow(3, 1 / 6) / m.gamma(1 / 3), 1e-14, "Bi'(0)");
    assert(m.airy(12).ai < 1e-12 && m.airy(12).bi > 1e10,
        "Ai decays and Bi grows on the positive axis");
    const keys = Object.keys(m.airy(1)).sort();
    assert(keys.join(",") === "ai,aip,bi,bip", "airy returns all four values");
}

{
    near(m.idivide(7, 2), 3, 0, "idivide default is fix (toward zero)");
    near(m.idivide(-7, 2), -3, 0, "fix on a negative truncates toward zero");
    near(m.idivide(-7, 2, "floor"), -4, 0, "floor rounds down");
    near(m.idivide(-7, 2, "ceil"), -3, 0, "ceil rounds up");
    near(m.idivide(-7, 2, "round"), -4, 0, "round is round-half-to-even");
    near(m.idivide(7, 2, "fix"), 3, 0, "explicit fix matches the default");
    throws(() => m.idivide(1, 2, "nearest"), "an unknown mode is refused");
    assert(!isFinite(m.idivide(1, 0)), "division by zero is IEEE, not a throw");

    const p3 = m.perms([1, 2, 3]);
    assert(p3.length === 6, "perms(3) has 3! rows");
    assert(JSON.stringify(p3) === JSON.stringify(
        [[3,2,1],[3,1,2],[2,3,1],[2,1,3],[1,3,2],[1,2,3]]),
        "perms is in reverse lexicographic order, as MATLAB emits it");
    assert(JSON.stringify(m.perms([9])) === "[[9]]", "one element is one row");
    assert(m.perms([]).length === 1, "the empty input has one (empty) permutation");
    assert(m.perms([1,2,3,4,5,6,7,8]).length === 40320, "8! rows");
    throws(() => m.perms([1,2,3,4,5,6,7,8,9]), "9 elements is refused");
    throws(() => m.perms("abc"), "a non-array is refused");
    const seen = new Set();
    for (const row of p3) {
        assert(row.length === 3, "each row has n entries");
        seen.add(row.join(","));
    }
    assert(seen.size === 6, "all six rows are distinct");
}

{
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
}

print("test_mathx_tierb: all " + n + " assertions passed");
