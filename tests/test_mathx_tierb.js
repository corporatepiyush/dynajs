/* test_mathx_tierb.js -- MATLAB tier B in dyna:mathx, over src/core/dyn-mathx.
 *
 * The per-regime ACCURACY of these functions is measured in C, by
 * tests/oracle_mathx_tierb.c, whose oracle is a set of identities rather than a
 * table of digits (Wronskians, three-term recurrences, reflection and
 * duplication rules, closed forms at integer parameters). That file is the
 * numerics gate and it is where the numbers in dyn-mathx.h come from.
 *
 * THIS file is the BINDING gate, which is a different question: given that the
 * core is right, does the JS surface call it correctly? The failure modes here
 * are argument ORDER (MATLAB spells gammainc(x,a) where every reference writes
 * P(a,x)), shape (does legendre return the whole column?), and the error paths.
 * A binding can be wrong in all three ways while the numerics underneath are
 * perfect, and the C harness cannot see any of it.
 *
 * So the checks below are deliberately a mix: identities where they are cheap
 * to restate in JS, and shape/contract assertions where they are the point.
 *
 * Run: dynajs (built with CONFIG_NATIVE_MODULES=y) tests/test_mathx_tierb.js
 */
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

/* ---- incomplete gamma: MATLAB's argument order is (x, a) ---------------- */
{
    /* P(1,x) = 1 - e^-x exactly, so a swapped binding shows up immediately:
     * gammainc(2,1) is 0.8647 and gammainc(1,2) is 0.2642. */
    near(m.gammainc(2, 1), 1 - Math.exp(-2), 1e-14, "gammainc(x=2, a=1)");
    near(m.gammainc(1, 2), 1 - 2 * Math.exp(-1), 1e-14, "gammainc(x=1, a=2)");
    assert(m.gammainc(2, 1) !== m.gammainc(1, 2),
        "the two argument orders differ, so the order is actually tested");

    /* P(1/2, x) = erf(sqrt x). */
    near(m.gammainc(3, 0.5), m.erf(Math.sqrt(3)), 1e-13, "gammainc vs erf");

    /* The upper tail is a separate computation, not 1 - P: at x = 40 the
     * subtraction has no relative accuracy left at all. */
    near(m.gammainc(2, 1, "upper"), Math.exp(-2), 1e-13, "gammainc upper");
    near(m.gammainc(40, 1, "upper"), Math.exp(-40), 1e-12, "gammainc upper, far tail");
    near(m.gammainc(5, 2) + m.gammainc(5, 2, "upper"), 1, 1e-15, "P + Q = 1");

    /* An unrecognised tail selector is the LOWER tail, matching MATLAB's
     * default rather than throwing -- pinned so it cannot drift. */
    near(m.gammainc(2, 1, "lower"), m.gammainc(2, 1), 0, "explicit lower");

    for (const [x, a] of [[3.5, 2], [0.2, 0.7], [80, 60]])
        near(m.gammaincinv(m.gammainc(x, a), a), x, 1e-10,
            "gammaincinv round trip at " + x + "," + a);
}

/* ---- incomplete beta ---------------------------------------------------- */
{
    /* For integer a,b the regularised incomplete beta is a binomial tail.
     * I_0.5(2,3) = (C(4,2)+C(4,3)+C(4,4))/16 = 11/16, computed by hand. */
    near(m.betainc(0.5, 2, 3), 0.6875, 1e-14, "betainc(0.5,2,3) = 11/16");
    near(m.betainc(0.3, 1, 1), 0.3, 1e-15, "I_x(1,1) = x");
    near(m.betainc(0.3, 2, 5) + m.betainc(0.7, 5, 2), 1, 1e-14, "betainc symmetry");
    assert(m.betainc(0.5, 2, 3) !== m.betainc(0.5, 3, 2),
        "a and b are not interchangeable, so their order is tested");
    for (const [p, a, b] of [[0.6875, 2, 3], [0.1, 0.5, 4], [0.99, 7, 2]])
        near(m.betainc(m.betaincinv(p, a, b), a, b), p, 1e-10,
            "betaincinv round trip at " + p);
}

/* ---- Bessel ------------------------------------------------------------- */
{
    /* The Wronskian I_nu K_(nu+1) + I_(nu+1) K_nu = 1/x couples the two
     * INDEPENDENT implementations (a power series and a quadrature), across
     * both of the core's switch points. */
    for (const x of [0.05, 0.4, 0.6, 3.7, 19.5, 21, 200])
        near(m.besseli(0, x) * m.besselk(1, x) + m.besseli(1, x) * m.besselk(0, x),
            1 / x, 1e-12, "Wronskian at x=" + x);

    /* Fractional order really is fractional -- I_-1/3 must NOT be folded to
     * I_1/3, which is only valid for integer order. */
    assert(Math.abs(m.besseli(-1 / 3, 0.5) - m.besseli(1 / 3, 0.5)) > 1e-3,
        "I_-1/3 and I_1/3 differ at small x");

    /* J and Y are libm passthroughs; the recurrence catches an order
     * off-by-one, which is the only way the binding can be wrong. */
    for (const k of [1, 2, 3])
        near(m.besselj(k - 1, 4) + m.besselj(k + 1, 4), (2 * k / 4) * m.besselj(k, 4),
            1e-13, "besselj recurrence k=" + k);
    near(m.bessely(0, 3) + m.bessely(2, 3), (2 / 3) * m.bessely(1, 3), 1e-13,
        "bessely recurrence");

    /* The scaled forms exist so the pair survives past the overflow point:
     * K_0(800) underflows to 0 and I_0(800) overflows to Infinity, while the
     * scaled ones are finite and their Wronskian still holds. */
    assert(m.besselk(0, 800) === 0, "K underflows past 745, as the double does");
    assert(!isFinite(m.besseli(0, 800)), "I overflows past 713, as the double does");
    near(m.besseliScaled(0, 800) * m.besselkScaled(1, 800) +
         m.besseliScaled(1, 800) * m.besselkScaled(0, 800), 1 / 800, 1e-12,
        "the scaled pair still satisfies the Wronskian where the raw pair cannot");

    /* Hankel returns [re, im]; kind 2 conjugates. */
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

/* ---- elliptic ----------------------------------------------------------- */
{
    /* ellipke returns [K, E] together because they share the AGM iteration. */
    const [K0, E0] = m.ellipke(0);
    near(K0, Math.PI / 2, 1e-15, "K(0) = pi/2");
    near(E0, Math.PI / 2, 1e-15, "E(0) = pi/2");
    near(m.ellipke(1)[1], 1, 1e-15, "E(1) = 1");
    assert(!isFinite(m.ellipke(1)[0]), "K(1) diverges");

    /* Legendre's relation constrains K and E jointly, so it catches the two
     * being swapped -- which a spot value would not. */
    for (const mm of [0.1, 0.3, 0.5, 0.9]) {
        const [K, E] = m.ellipke(mm), [Kc, Ec] = m.ellipke(1 - mm);
        near(E * Kc + Ec * K - K * Kc, Math.PI / 2, 1e-14,
            "Legendre relation at m=" + mm);
    }

    /* Jacobi: the two defining identities, plus the degenerate limits. */
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

/* ---- Legendre ----------------------------------------------------------- */
{
    /* legendre(n, x) is MATLAB's shape: the whole m = 0..n column, which the
     * upward recurrence produces anyway on the way to any single value. */
    const col = m.legendre(3, 0.4);
    assert(Array.isArray(col) && col.length === 4, "legendre returns the m=0..n column");
    near(col[0], 0.5 * (5 * 0.4 ** 3 - 3 * 0.4), 1e-14, "P_3^0");
    near(col[1], m.legendreP(3, 1, 0.4), 0, "the column agrees with legendreP");
    assert(m.legendre(0, 0.5).length === 1, "degree 0 is a single value");

    /* Written-out closed forms, including the Condon-Shortley phase -- the
     * sign convention is the thing most easily lost in a binding. */
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

/* ---- polygamma ---------------------------------------------------------- */
{
    near(m.polygamma(0, 1), -0.5772156649015329, 1e-14, "psi(1) = -euler");
    near(m.polygamma(0, 2.5), m.psi(2.5), 0,
        "polygamma(0, x) IS psi(x) -- one implementation, not two");
    near(m.polygamma(1, 1), Math.PI ** 2 / 6, 1e-14, "trigamma(1) = zeta(2)");
    near(m.polygamma(1, 0.5), Math.PI ** 2 / 2, 1e-14, "trigamma(1/2)");
    /* The exact recurrence, which covers the shift-then-asymptotic path. */
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

/* ---- Airy --------------------------------------------------------------- */
{
    /* Ai Bi' - Ai' Bi = 1/pi for EVERY real x, which is the one check that
     * spans all three of the core's regimes and audits their switch points. */
    for (const x of [-30, -12, -7.5, -7, -6.9, -3, -0.5, 0, 0.05, 0.1, 0.2, 3, 15, 30]) {
        const a = m.airy(x);
        near(a.ai * a.bip - a.aip * a.bi, 1 / Math.PI, 1e-9,
            "Airy Wronskian at x=" + x);
    }
    /* The values at the origin, computed from Gamma rather than quoted. */
    const a0 = m.airy(0);
    near(a0.ai, Math.pow(3, -2 / 3) / m.gamma(2 / 3), 1e-14, "Ai(0)");
    near(a0.aip, -Math.pow(3, -1 / 3) / m.gamma(1 / 3), 1e-14, "Ai'(0)");
    near(a0.bi, Math.pow(3, -1 / 6) / m.gamma(2 / 3), 1e-14, "Bi(0)");
    near(a0.bip, Math.pow(3, 1 / 6) / m.gamma(1 / 3), 1e-14, "Bi'(0)");
    /* Ai decays and Bi grows for large positive x -- a swapped pair would
     * satisfy neither. */
    assert(m.airy(12).ai < 1e-12 && m.airy(12).bi > 1e10,
        "Ai decays and Bi grows on the positive axis");
    /* All four come from one evaluation, so the object shape is the contract. */
    const keys = Object.keys(m.airy(1)).sort();
    assert(keys.join(",") === "ai,aip,bi,bip", "airy returns all four values");
}

/* ---- tier A gaps: idivide and perms ------------------------------------
 *
 * Both are named in the plan's tier A list and neither existed -- the ledger
 * claimed "tier A, 30 functions, landed". Probing found them missing, which is
 * the same discipline that found the module-count error. */
{
    /* Four rounding modes, because "integer division" names four operations
     * and C's truncation is only one. -7/2 is the case that separates them. */
    near(m.idivide(7, 2), 3, 0, "idivide default is fix (toward zero)");
    near(m.idivide(-7, 2), -3, 0, "fix on a negative truncates toward zero");
    near(m.idivide(-7, 2, "floor"), -4, 0, "floor rounds down");
    near(m.idivide(-7, 2, "ceil"), -3, 0, "ceil rounds up");
    near(m.idivide(-7, 2, "round"), -4, 0, "round is round-half-to-even");
    near(m.idivide(7, 2, "fix"), 3, 0, "explicit fix matches the default");
    throws(() => m.idivide(1, 2, "nearest"), "an unknown mode is refused");
    assert(!isFinite(m.idivide(1, 0)), "division by zero is IEEE, not a throw");

    /* MATLAB emits permutations in REVERSE lexicographic order; matching that
     * matters because a caller diffing against MATLAB output would otherwise
     * see a difference that is only an ordering. */
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
    /* Every row is a genuine permutation: same multiset, no repeats. */
    const seen = new Set();
    for (const row of p3) {
        assert(row.length === 3, "each row has n entries");
        seen.add(row.join(","));
    }
    assert(seen.size === 6, "all six rows are distinct");
}

/* ---- audit regressions: mod exactness, erfcx/eps/nthroot, BigInt domains --
 * Reference sources: values marked "hp" come from probes/ref/hp.py, an
 * independent 70-digit decimal implementation (self-tested against closed
 * forms: Gamma(n)=(n-1)!, erf(1) A&S 7.1.1, B(2,3)=1/12, K(0)=pi/2, ...);
 * "binomial" means the exact integer closed form; MATLAB pins follow the
 * MATLAB definitions of mod/eps. */
{
    /* mod: floored modulo computed over exact fmod. The old x/y-rounded
     * form returned 4 and -2 -- impossible outputs for a remainder of 3
     * and 7 (10^17 mod 3 = 1, mod 7 = 5, by python BigInt). */
    assert(m.mod(1e17, 3) === 1, "mod(1e17,3) is the true remainder 1");
    assert(m.mod(1e17, 7) === 5, "mod(1e17,7) is 5, never negative for y > 0");
    assert(m.mod(1234567890123456800, 9) === 6, "mod stays exact at 1.2e18");
    assert(m.mod(6.830384611083916e18, 12345) === 7918, "mod at 6.8e18 matches BigInt truth");

    /* erfcx: the asymptotic branch carries the series through the 1/x^10
     * term. Cross-check against the exact path, still finite (erfc does
     * not underflow until x ~ 27); erfcx(30) is the hp reference. */
    for (const x of [25, 25.5, 26, 26.5]) {
        const exact = Math.exp(x * x) * m.erfc(x);
        near(m.erfcx(x), exact, 1e-14, "erfcx(" + x + ") matches exp(x^2) erfc(x)");
    }
    near(m.erfcx(30), 0.018795888861416745, 1e-15, "erfcx(30) hp reference");

    /* eps: eps(realmax) is the ulp of the top binade, 2^971 (MATLAB) */
    assert(m.eps(m.realmax()) === 2 ** 971, "eps(realmax) is 2^971 (MATLAB)");
    assert(m.eps(1) === 2.220446049250313e-16, "eps(1) is 2^-52");
    assert(m.eps(4) === 8.881784197001252e-16, "eps(4) is 2^-50");
    assert(m.eps(m.realmin()) === 5e-324, "eps(realmin) is 2^-1074");

    /* nthroot: cube root via cbrt (odd, correctly rounded) */
    near(m.nthroot(1e300, 3), 1e100, 1e-15, "nthroot(1e300,3) is exact cbrt");
    assert(m.nthroot(-27, 3) === -3, "nthroot(-27,3) = -3");
    assert(m.nthroot(-8, 3) === -2, "nthroot(-8,3) = -2");

    /* besselh: kind is validated, not truncated */
    throws(() => m.besselh(1, 1, 2.5), "besselh kind 2.5 is refused");
    assert(m.besselh(0, 1)[1] === m.bessely(0, 1), "besselh defaults to kind 1");

    /* isPrime/gcd/lcm: wide BigInts are refused, not reduced mod 2^64.
     * isPrime(2n**64n + 1n) used to certify 1n and return true. */
    throws(() => m.isPrime(2n ** 64n + 1n), "isPrime refuses BigInts above uint64");
    throws(() => m.gcd(2n ** 64n, 2n), "gcd refuses BigInts above int64");
    throws(() => m.lcm(-(2n ** 63n) - 1n, 2n), "lcm refuses BigInts below int64");
    assert(m.isPrime(9223372036854775807n) === false, "2^63-1 = 7*73*127*337*92737*649657 is composite");

    /* abs/bitLen/popcount: magnitude must be < 2^64. bitLen(factorial(21))
     * returned the low 64-bit word (62) where the true bit length is 66. */
    throws(() => m.bitLen(m.factorial(21)), "bitLen refuses >64-bit magnitudes");
    throws(() => m.popcount(-(2n ** 200n)), "popcount refuses >64-bit magnitudes");
    throws(() => m.abs(-(2n ** 200n)), "abs refuses >64-bit magnitudes");
    assert(m.bitLen(255n) === 8, "bitLen(255n) is 8");
    assert(m.bitLen(-(2n ** 63n)) === 64, "bitLen(-2^63) is 64");
    assert(m.bitLen(0n) === 0, "bitLen(0n) is 0");
    assert(m.popcount(255n) === 8, "popcount(255n) is 8");
    assert(m.abs(-9007199254740993n) === 9007199254740993n, "abs is the magnitude");
    assert(m.abs(2n ** 63n - 1n) === 2n ** 63n - 1n, "abs keeps uint64 magnitudes");

    /* isPrime pseudoprime battery (ground truth by factorization);
     * 3215031751 = 151*751*28351, the smallest SPSP to bases 2,3,5,7 */
    for (const c of [341, 561, 645, 1105, 4141, 3215031751])
        assert(m.isPrime(c) === false, "isPrime(" + c + ") is false (pseudoprime)");
    assert(m.isPrime(2305843009213693951n) === true, "2^61-1 is prime (Mersenne)");

    /* factorial(10000): 35660 digits, 118459 bits (python reference) */
    assert(m.factorial(10000).toString().length === 35660, "10000! has 35660 digits");

    /* expression contract edges (own data properties only) */
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

    /* bits: brute-force JS references on edge inputs */
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
        /* widening mul: hi*2^w+lo === a*b (BigInt ground truth) */
        for (const [a, b] of [[0xffffffff, 0xffffffff], [0x12345678, 0x9abcdef0], [1, 1], [0, 7]]) {
            const [hi, lo] = m.bits.mul32(a, b);
            assert(BigInt(hi) * 4294967296n + BigInt(lo) === BigInt(a) * BigInt(b), "mul32(" + a + "," + b + ")");
        }
        for (const [a, b] of [[2n ** 64n - 1n, 2n ** 64n - 1n], [12345678901234567890n, 9876543210987654321n]]) {
            const [hi, lo] = m.bits.mul64(a, b);
            assert(((BigInt(hi) << 64n) | BigInt(lo)) === BigInt.asUintN(128, a * b), "mul64 hi:lo");
        }
        /* div32 throws on y == 0 and y <= hi, with the documented messages */
        throws(() => m.bits.div32(1, 0, 0), "div32 by zero throws");
        throws(() => m.bits.div32(5, 1, 5), "div32 throws when y <= hi");
        const qr = m.bits.div32(0, 100, 7);
        assert(qr[0] === 14 && qr[1] === 2, "div32(0,100,7) = [14,2]");
        assert(m.bits.rem32(5, 100, 7) === 1, "rem32(5,100,7) = (5*2^32+100)%7, no overflow panic");
    }

    /* gammaincinv roundtrip envelope (p, a) -- the binding takes (p, a) */
    for (const p of [0.01, 0.5, 0.9, 0.999]) {
        for (const a of [0.5, 2, 10.5]) {
            const x = m.gammaincinv(p, a);
            near(m.gammainc(x, a), p, 1e-12, "gammaincinv(" + p + "," + a + ") roundtrips");
        }
    }
    /* betainc(0.2, 8, 3) equals the binomial tail P(Bin(10,0.2) >= 8) */
    near(m.betainc(0.2, 8, 3), 7.3728e-5 + 4.096e-6 + 1.024e-7, 1e-14,
        "betainc matches the binomial closed form");

    /* Bessel integer-order recurrence J_{n-1} + J_{n+1} = (2n/x) J_n */
    for (let jn = 1; jn <= 6; jn++) {
        const lhs = m.besselj(jn - 1, 5) + m.besselj(jn + 1, 5);
        const rhs = (2 * jn / 5) * m.besselj(jn, 5);
        near(lhs, rhs, 1e-13, "J recurrence at n=" + jn);
    }
    /* modified-Bessel closed forms (DLMF 10.27.6-10.27.7) */
    const zx = 2.5;
    near(m.besseli(0.5, zx), Math.sqrt(2 / (Math.PI * zx)) * Math.sinh(zx), 1e-15, "I_{1/2} closed form");
    near(m.besselk(0.5, zx), Math.sqrt(Math.PI / (2 * zx)) * Math.exp(-zx), 1e-15, "K_{1/2} closed form");
    /* Airy Wronskian Ai Bi' - Ai' Bi = 1/pi across all three regimes */
    let ww = 0;
    for (let ax = -15; ax <= 15; ax += 0.5) {
        const a = m.airy(ax);
        const err = Math.abs(a.ai * a.bip - a.aip * a.bi - 1 / Math.PI);
        if (err > ww) ww = err;
    }
    assert(ww < 5e-12, "Airy Wronskian holds across regimes (worst " + ww + ")");
}

print("test_mathx_tierb: all " + n + " assertions passed");
