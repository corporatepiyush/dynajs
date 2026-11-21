// covers: contract dyna:decimal.Decimal.div dyna:decimal.Decimal.divmod dyna:decimal.Decimal.mod dyna:decimal.Decimal.mul dyna:decimal.Decimal.add dyna:decimal.Decimal.sub -- the shared digit kernels changed together: long division, limb multiply, aligned add/sub
// decnum keeps one decimal digit per byte. Long division now divides by a divisor of up to 18 digits as a machine
// integer (9 numerator digits per 64-bit division when the divisor is below 10^9, one digit per division up to 10^18)
// and, for longer divisors, slides the remainder window instead of shifting it every digit and starts each quotient
// digit from floor(top 18 remainder digits / (top 17 divisor digits + 1)), which is never above the true digit, with
// one multiply-subtract (the old trial subtractions finish it). Multiplication packs 4 digits per limb (64-bit
// accumulators, stack buffers up to 64 limbs). add/sub index both operands directly. dec_div/dec_int_divmod use one
// stack buffer up to 512 bytes. All of it is exact integer work, so the oracle is BigInt: a/b at precision p in each
// rounding mode (sticky remainder included), truncated divmod/mod, products, sums and differences, over 1..60 digit
// operands with exponents -15..15, divisors around 10^9 and 10^18 boundaries, and divisors like 1999...9 / 1000...01
// where the leading-digit estimate is one short.
// F-dec-1 (fixed): an inexact quotient rounded away from zero (up, or ceil/floor by sign) whose first p digits end in 0
// was rounded at its last NONZERO digit instead of the p-th (9/999999999 at precision 60, rounding up, had 54 digits):
// dec_div trims trailing zeros before dec_round_sig, which then added the unit at the lowest stored digit. dec_round_sig
// now pads a sticky short coefficient back to p digits first. Those cases are asserted like the rest and counted; the
// last section pins the reported quotient against Python decimal (Context(prec=60, rounding=ROUND_UP).divide(9, 999999999)).
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_decimal_decimal_div_longdiv_oracle");
import { Decimal } from "dyna:decimal";

let checks = 0, fails = 0;
let seed = 97;
const rnd = () => ((seed = (Math.imul(seed, 1103515245) + 12345) >>> 0) / 4294967296);
const P10 = (n) => 10n ** BigInt(n);
const absB = (x) => (x < 0n ? -x : x);
const ndig = (x) => (x === 0n ? 0 : absB(x).toString().length);
const str = (c, e) => c.toString() + "e" + e;
function eq(got, wantC, wantE, what) {
    checks++;
    let ok;
    try { ok = got.equals(new Decimal(str(wantC, wantE))); } catch (e) { ok = false; }
    if (!ok) { fails++; if (fails < 20) print("FAIL: " + what + ": got " + got + ", want " + str(wantC, wantE)); }
}
function roundCut(c, cut, mode, sticky) {
    if (cut <= 0) return c;
    const neg = c < 0n, a = absB(c), d = P10(cut);
    let q = a / d;
    const rem = a % d, half = 5n * P10(cut - 1);
    const cmp = rem > half || (rem === half && sticky) ? 1 : rem === half ? 0 : -1;
    const inexact = rem > 0n || sticky;
    let up = false;
    if (mode === "up") up = inexact;
    else if (mode === "ceil") up = inexact && !neg;
    else if (mode === "floor") up = inexact && neg;
    else if (mode === "halfUp") up = cmp >= 0;
    else if (mode === "halfDown") up = cmp > 0;
    else if (mode === "halfEven") up = cmp > 0 || (cmp === 0 && q % 2n === 1n);
    if (up) q += 1n;
    return neg ? -q : q;
}
function oracleDiv(ac, ae, bc, be, p, mode) {
    const k = p + 3 + ndig(bc);
    const num = ac * P10(k);
    const q = num / bc, r = num % bc;
    const cut = ndig(q) - p;
    if (cut <= 0) {
        if (r === 0n) return [q, ae - be - k];
        const k2 = k + (1 - cut) + 2;
        return oracleDivK(ac, ae, bc, be, p, mode, k2);
    }
    return [roundCut(q, cut, mode, r !== 0n), ae - be - k + cut];
}
function oracleDivK(ac, ae, bc, be, p, mode, k) {
    const num = ac * P10(k), q = num / bc, r = num % bc, cut = ndig(q) - p;
    return [roundCut(q, cut, mode, r !== 0n), ae - be - k + cut];
}
function digs(n) { let s = String(1 + ((rnd() * 9) | 0)); for (let i = 1; i < n; i++) s += (rnd() * 10) | 0; return s; }
function operand(maxd) {
    const c = BigInt((rnd() < 0.4 ? "-" : "") + digs(1 + ((rnd() * maxd) | 0)));
    return [c, ((rnd() * 31) | 0) - 15];
}
const modes = ["halfEven", "halfUp", "halfDown", "up", "down", "ceil", "floor"];
function f1Pattern(ac, ae, bc, be, p, mode) {
    const [tc] = oracleDiv(ac, ae, bc, be, p, "down"), [rc] = oracleDiv(ac, ae, bc, be, p, mode);
    return tc !== rc && tc % 10n === 0n;
}
let f1Cases = 0;
function checkPair(ac, ae, bc, be, p, mode, tag) {
    const a = new Decimal(str(ac, ae)), b = new Decimal(str(bc, be));
    const [qc, qe] = oracleDiv(ac, ae, bc, be, p, mode);
    if (f1Pattern(ac, ae, bc, be, p, mode)) f1Cases++;
    eq(a.div(b, { precision: p, rounding: mode }), qc, qe, tag + " div p=" + p + " " + mode + " " + str(ac, ae) + "/" + str(bc, be));
    const lo = Math.min(ae, be), A = ac * P10(ae - lo), B = bc * P10(be - lo);
    if (ndig(A) * ndig(B) < 3000000) {
        const q = A / B, r = A % B;
        const [gq, gr] = a.divmod(b);
        eq(gq, q, 0, tag + " divmod q");
        eq(gr, r, lo, tag + " divmod r");
        eq(a.mod(b), r, lo, tag + " mod");
    }
    eq(a.mul(b), ac * bc, ae + be, tag + " mul");
    eq(a.add(b), A + B, lo, tag + " add");
    eq(a.sub(b), A - B, lo, tag + " sub");
}
__kitS.section("random operands, every precision and rounding mode", () => {
    for (let i = 0; i < 1500; i++) {
        const [ac, ae] = operand(rnd() < 0.5 ? 25 : 60), [bc, be] = operand([3, 9, 10, 18, 19, 25, 60][(rnd() * 7) | 0]);
        checkPair(ac, ae, bc, be, [1, 5, 16, 34, 50, 100][(rnd() * 6) | 0], modes[(rnd() * 7) | 0], "rand");
    }
});
__kitS.section("divisors at the 10^9 / 10^18 limits and estimate-short divisors", () => {
    const divs = ["999999999", "1000000000", "1000000001", "999999999999999999", "1000000000000000000", "1000000000000000001",
        "19999999999999999999", "10000000000000000001", "99999999999999999999999", "10000000000000000000000000000000000001",
        "1999999999999999999999999999999999", "123456789012345678901234567890123456789", "7", "1", "3"];
    for (const ds of divs) for (const n of [1, 9, 18, 19, 40, 90]) for (const sgn of [1n, -1n]) {
        const ac = sgn * BigInt(digs(n)), bc = BigInt(ds);
        checkPair(ac, 0, bc, 0, 34, "halfEven", "edge " + ds);
        checkPair(ac, 3, bc, -2, 50, "floor", "edgeExp " + ds);
        checkPair(BigInt("9".repeat(n)), 0, bc, 0, 60, "up", "nines " + ds);
    }
});
__kitS.section("long products and sums beyond the 64-limb stack buffer", () => {
    for (const [n, m] of [[255, 3], [256, 256], [257, 300], [400, 41], [1, 1], [4, 4], [5, 3]]) {
        const ac = BigInt(digs(n)), bc = -BigInt(digs(m));
        eq(new Decimal(str(ac, -7)).mul(new Decimal(str(bc, 4))), ac * bc, -3, "mul " + n + "x" + m);
        eq(new Decimal(str(ac, -7)).add(new Decimal(str(bc, 4))), ac + bc * P10(11), -7, "add " + n + "+" + m);
        const [qc, qe] = oracleDiv(ac, -7, bc, 4, 200, "halfEven");
        eq(new Decimal(str(ac, -7)).div(new Decimal(str(bc, 4)), { precision: 200 }), qc, qe, "div200 " + n + "/" + m);
    }
});
__kitS.section("F-dec-1: 9/999999999 at precision 60 rounds at the 60th digit (Python decimal oracle)", () => {
    const PY = "9.00000000900000000900000000900000000900000000900000000900001E-9";
    const PY_TRUNC = "9.00000000900000000900000000900000000900000000900000000900000E-9";
    const cases = [["9", "up", PY], ["-9", "up", "-" + PY], ["9", "ceil", PY], ["-9", "ceil", "-" + PY_TRUNC],
        ["9", "floor", PY_TRUNC], ["-9", "floor", "-" + PY], ["9", "down", PY_TRUNC]];
    for (const [a, mode, want] of cases) {
        checks++;
        const got = new Decimal(a).div("999999999", { precision: 60, rounding: mode });
        const sig = got.toString().replace(/[-.]/g, "").replace(/^0+/, "").length;
        if (!got.equals(new Decimal(want)) || (want.endsWith("900001E-9") && sig !== 60)) {
            fails++;
            print("FAIL: " + a + "/999999999 p=60 " + mode + ": got " + got + ", want " + want);
        }
    }
    eq(new Decimal("1").div("3", { precision: 5, rounding: "up" }), 33334n, -5, "1/3 p=5 up (Python 0.33334)");
    eq(new Decimal("10").div("9999", { precision: 5, rounding: "up" }), 10002n, -7, "10/9999 p=5 up (Python 0.0010002)");
});
if (fails) throw new Error(fails + " of " + checks + " checks failed");
print("test_decimal_decimal_div_longdiv_oracle: " + checks + " checks passed (" + f1Cases + " of them directed-rounding quotients whose p-digit truncation ends in 0: F-dec-1)");
__kitS.end();
