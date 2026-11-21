// covers: contract dyna:decimal.Decimal.ln dyna:decimal.Decimal.log10 dyna:decimal.Decimal.exp dyna:decimal.Decimal.sqrt -- the series kernels changed together: cached ln2/ln10, small-divisor long division, limb multiply, one sqrt scratch buffer
// ln/log10 used to recompute ln2 (atanh(1/3)) and ln10 (3 ln2 + 2 atanh(1/9)) at the working precision on every
// call; each thread now keeps the last ln2 and the last ln10 per working precision (up to 256 digits, 2 x ~270 B, no
// heap, so nothing to free at thread exit). The series divide by small integers (2k+1, k) through the machine-integer
// long division, multiply through 4-digit limbs, and sqrt allocates its trial-product buffer once. Correctly rounded
// results must not move, so the oracle recomputes them with BigInt fixed point (precision + 60 guard digits:
// ln via powers of ten/two and atanh, exp via halving + Taylor + squaring, sqrt via integer Newton) and rounds half-even.
// Precisions are interleaved (5, 16, 34, 50, 120, 300) so the per-precision cache is hit, replaced and bypassed
// (300 > 256 digits) in every order; ln(2^k 10^E), log10(10^E) (exact) and ln/log10/exp/sqrt of random values.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_decimal_decimal_ln_series_oracle");
import { Decimal } from "dyna:decimal";

let checks = 0, fails = 0;
let seed = 1234;
const rnd = () => ((seed = (Math.imul(seed, 1103515245) + 12345) >>> 0) / 4294967296);
const P10 = (n) => 10n ** BigInt(n);
const ndig = (x) => (x === 0n ? 0 : (x < 0n ? -x : x).toString().length);
function roundHE(c, e, p) {
    const neg = c < 0n, a = neg ? -c : c, cut = ndig(a) - p;
    if (cut <= 0) return [c, e];
    const d = P10(cut), half = 5n * P10(cut - 1);
    let q = a / d;
    const rem = a % d;
    if (rem > half || (rem === half && q % 2n === 1n)) q += 1n;
    return [neg ? -q : q, e + cut];
}
function eq(got, c, e, what) {
    checks++;
    let ok;
    try { ok = got.equals(new Decimal(c.toString() + "e" + e)); } catch (x) { ok = false; }
    if (!ok) { fails++; if (fails < 20) print("FAIL: " + what + ": got " + got + ", want " + c + "e" + e); }
}
const cache = new Map();
function atanhInv(n, S) {
    const nn = n * n;
    let term = S / n, sum = term, k = 1n;
    while (term !== 0n) { term /= nn; sum += term / (2n * k + 1n); k++; }
    return sum;
}
function consts(G) {
    if (cache.has(G)) return cache.get(G);
    const S = P10(G), ln2 = 2n * atanhInv(3n, S), ln10 = 3n * ln2 + 2n * atanhInv(9n, S);
    const r = { S, ln2, ln10 };
    cache.set(G, r);
    return r;
}
function atanhFrac(num, den, S) {
    let term = (S * num) / den, sum = term, k = 1n;
    const n2 = num * num, d2 = den * den;
    while (term !== 0n) { term = (term * n2) / d2; sum += term / (2n * k + 1n); k++; }
    return sum;
}
function lnFix(c, e, G) {
    const { S, ln2, ln10 } = consts(G);
    const E = ndig(c) - 1 + e;
    let N = c, D = 1n;
    const sh = e - E;
    if (sh >= 0) N *= P10(sh); else D *= P10(-sh);
    let k = 0n;
    while (N >= 2n * D) { D *= 2n; k++; }
    return BigInt(E) * ln10 + k * ln2 + 2n * atanhFrac(N - D, N + D, S);
}
function expFix(c, e, G) {
    const S = P10(G);
    let x = e >= 0 ? c * P10(e) * S : (c * S) / P10(-e);
    let n = 0;
    const lim = S / 8n;
    while ((x < 0n ? -x : x) > lim) { x /= 2n; n++; }
    let term = S, sum = S, k = 1n;
    while (term !== 0n) { term = (term * x) / S / k; sum += term; k++; }
    for (let i = 0; i < n; i++) sum = (sum * sum) / S;
    return sum;
}
function isqrt(n) {
    if (n < 2n) return n;
    let x = 1n << BigInt((n.toString(2).length >> 1) + 1);
    for (;;) { const y = (x + n / x) >> 1n; if (y >= x) break; x = y; }
    while (x * x > n) x--;
    while ((x + 1n) * (x + 1n) <= n) x++;
    return x;
}
function checkLn(c, e, p) {
    const G = p + 60, L = lnFix(c, e, G);
    const [rc, re] = roundHE(L, -G, p);
    const x = new Decimal(c + "e" + e);
    eq(x.ln({ precision: p }), rc, re, "ln(" + c + "e" + e + ") p=" + p);
    const { ln10, S } = consts(G);
    const [lc, le] = roundHE((L * S) / ln10, -G, p);
    eq(x.log10({ precision: p }), lc, le, "log10(" + c + "e" + e + ") p=" + p);
}
function checkExp(c, e, p) {
    const G = p + 60, X = expFix(c, e, G);
    const [rc, re] = roundHE(X, -G, p);
    eq(new Decimal(c + "e" + e).exp({ precision: p }), rc, re, "exp(" + c + "e" + e + ") p=" + p);
}
function checkSqrt(c, e, p) {
    let t = 0;
    while (e + 2 * t < 0 || ndig(c) + e + 2 * t < 2 * p + 6) t++;
    const N = c * P10(e + 2 * t), s = isqrt(N);
    const [rc, re] = s * s === N ? roundHE(s, -t, p) : roundHE(s * 10n + 1n, -t - 1, p);
    eq(new Decimal(c + "e" + e).sqrt({ precision: p }), rc, re, "sqrt(" + c + "e" + e + ") p=" + p);
}
const precs = [5, 16, 34, 50, 120, 300];
const digs = (n) => { let s = String(1 + ((rnd() * 9) | 0)); for (let i = 1; i < n; i++) s += (rnd() * 10) | 0; return BigInt(s); };
__kitS.section("ln2, ln10 and ln(2^k 10^E) across interleaved precisions", () => {
    for (let r = 0; r < 3; r++) for (const p of r === 1 ? [...precs].reverse() : precs) {
        checkLn(2n, 0, p);
        checkLn(10n, 0, p);
        checkLn(2n ** 13n, -5, p);
        checkLn(5n, 7, p);
    }
    for (const E of [-30, -1, 1, 2, 77]) for (const p of [16, 34]) eq(new Decimal("1e" + E).log10({ precision: p }), BigInt(E), 0, "log10 1e" + E);
});
__kitS.section("ln/log10 of random values, precisions interleaved", () => {
    for (let i = 0; i < 80; i++) checkLn(digs(1 + ((rnd() * 40) | 0)), ((rnd() * 40) | 0) - 20, precs[(rnd() * 5) | 0]);
});
__kitS.section("exp of random values", () => {
    for (let i = 0; i < 80; i++) {
        const c = digs(1 + ((rnd() * 12) | 0)) * (rnd() < 0.5 ? -1n : 1n);
        const e = -((rnd() * 12) | 0) - Math.max(0, ndig(c) - 2);
        checkExp(c, e, [5, 16, 34, 50][(rnd() * 4) | 0]);
    }
});
__kitS.section("sqrt of random values", () => {
    for (let i = 0; i < 150; i++) checkSqrt(digs(1 + ((rnd() * 60) | 0)), ((rnd() * 41) | 0) - 20, precs[(rnd() * 6) | 0]);
});
if (fails) throw new Error(fails + " of " + checks + " checks failed");
print("test_decimal_decimal_ln_series_oracle: " + checks + " checks passed");
__kitS.end();
