// dynajs_mathx.js — MATLAB tier B special functions in dyna:mathx.
//
// These are the functions whose accuracy depends on WHERE in the argument plane
// you evaluate them. Each switches algorithm at a documented threshold, and the
// interesting cases are the SEAMS — a caller sweeping an argument crosses them
// without knowing.
//
// BEST  — a regime where the method is strong (the Bessel asymptotic past x=20;
//         Airy's positive axis, where everything is a positive combination of
//         positive Bessel values and nothing cancels).
// WORST — the Airy Maclaurin series approaching x = -7, which cancels terms of
//         size e^zeta against a result of order 1 and loses about four digits.
//         That band is published, not smoothed over.
//
// Run: dynajs examples/js/dynajs_mathx.js
import { test, run, assert } from "./harness.js";
import * as m from "dyna:mathx";

const near = (a, b, tol, msg) => {
  if (!(Math.abs(a - b) <= tol * Math.max(1, Math.abs(b))))
    throw new Error(`${msg}: ${a} vs ${b}`);
};

test("identities hold, and that is how these are checked", () => {
  // Not a table of digits — an identity cannot be misremembered in a way that
  // accidentally passes. P(1,x) = 1 - e^-x, exactly.
  near(m.gammainc(2, 1), 1 - Math.exp(-2), 1e-14, "gammainc vs its closed form");
  // I_x(2,3) = 11/16, from the binomial tail. Computed by hand.
  near(m.betainc(0.5, 2, 3), 0.6875, 1e-14, "betainc vs the binomial tail");
  // Legendre's relation constrains K and E jointly, so it catches them swapped.
  const [K, E] = m.ellipke(0.3), [Kc, Ec] = m.ellipke(0.7);
  near(E * Kc + Ec * K - K * Kc, Math.PI / 2, 1e-14, "Legendre's relation");
});

test("BEST: the Wronskian holds to 1e-13 across both Bessel seams", () => {
  // I_nu K_(nu+1) + I_(nu+1) K_nu = 1/x, exactly, for every order and argument.
  // It couples a power series against a quadrature, so neither can be wrong
  // without the other being wrong in exactly the compensating way.
  let worst = 0;
  for (const x of [0.05, 0.4, 0.6, 3.7, 19.5, 21, 200]) {
    const w = m.besseli(0, x) * m.besselk(1, x) + m.besseli(1, x) * m.besselk(0, x);
    worst = Math.max(worst, Math.abs(w - 1 / x) * x);
  }
  print(`  Bessel Wronskian worst relative error: ${worst.toExponential(2)}`);
  assert(worst < 1e-13, "across the x=0.5 and x=20 switch points");
});

test("BEST vs WORST: the Airy Wronskian, by regime", () => {
  // Ai Bi' - Ai' Bi = 1/pi for EVERY real x — the one check that spans all
  // three regimes and audits their switch points.
  const band = { "x>=0.1 (Bessel)": 0, "-7..0.1 (series)": 0, "x<-7 (asymptotic)": 0 };
  for (let x = -30; x <= 30; x += 0.05) {
    const a = m.airy(x);
    const e = Math.abs((a.ai * a.bip - a.aip * a.bi) * Math.PI - 1);
    const k = x >= 0.1 ? "x>=0.1 (Bessel)" : x >= -7 ? "-7..0.1 (series)" : "x<-7 (asymptotic)";
    band[k] = Math.max(band[k], e);
  }
  for (const k of Object.keys(band)) print(`  ${k.padEnd(20)} ${band[k].toExponential(2)}`);
  // The series regime IS the weak one — the opposite of what one would guess,
  // and the reason it is published.
  assert(band["-7..0.1 (series)"] > band["x<-7 (asymptotic)"],
    "the Maclaurin series, not the transition band, is the weak regime");
  assert(band["-7..0.1 (series)"] < 1e-10, "and it is still good to ten digits");
});

test("the scaled forms exist because the raw pair cannot survive", () => {
  // K underflows past 745 and I overflows past 713, exactly as a double must.
  assert(m.besselk(0, 800) === 0, "K underflows");
  assert(!isFinite(m.besseli(0, 800)), "I overflows");
  // The scaled pair is finite there and still satisfies the Wronskian.
  const w = m.besseliScaled(0, 800) * m.besselkScaled(1, 800) +
            m.besseliScaled(1, 800) * m.besselkScaled(0, 800);
  near(w, 1 / 800, 1e-12, "the scaled pair still holds where the raw one cannot");
});

test("MATLAB's spellings, including the ones that differ", () => {
  // gammainc(x, a) — the variable first, which is NOT the reference order.
  assert(m.gammainc(2, 1) !== m.gammainc(1, 2), "the argument order is real");
  // legendre(n, x) returns the whole m = 0..n column, as MATLAB does.
  assert(m.legendre(3, 0.4).length === 4, "the whole column");
  // P_1^1 carries the Condon-Shortley minus — the convention most easily lost.
  near(m.legendreP(1, 1, 0.4), -Math.sqrt(1 - 0.16), 1e-15, "Condon-Shortley phase");
  // idivide names its rounding mode, because "integer division" is four
  // operations and C's truncation is only one.
  print(`  idivide(-7,2): fix ${m.idivide(-7, 2)}  floor ${m.idivide(-7, 2, "floor")}` +
        `  ceil ${m.idivide(-7, 2, "ceil")}  round ${m.idivide(-7, 2, "round")}`);
  // perms is in MATLAB's reverse-lexicographic order.
  assert(JSON.stringify(m.perms([1, 2, 3])[0]) === "[3,2,1]", "reverse lex, as MATLAB emits");
});

test("abuse: out-of-domain arguments are refused, not guessed", () => {
  const throws = (fn) => { try { fn(); return false; } catch { return true; } };
  assert(throws(() => m.besselj(0.5, 1)), "fractional order to besselj");
  assert(throws(() => m.ellipj(1, 2)), "m outside [0,1]");
  assert(throws(() => m.polygamma(-1, 1)), "negative polygamma order");
  assert(throws(() => m.legendre(2.5, 0.4)), "fractional degree");
  assert(throws(() => m.besselh(1, 1, 3)), "a Hankel kind that is not 1 or 2");
  assert(throws(() => m.perms([1,2,3,4,5,6,7,8,9])), "9! rows is refused");
  assert(throws(() => m.idivide(1, 2, "nearest")), "an unknown rounding mode");

  // NaN where the value genuinely does not exist, rather than a made-up number.
  assert(Number.isNaN(m.legendreP(2, 3, 0.4)), "order above degree");
  assert(Number.isNaN(m.legendreP(2, 0, 1.5)), "|x| > 1");
  assert(Number.isNaN(m.polygamma(1, -2)), "a pole");
  // A negative NON-integer polygamma argument is NaN deliberately: the
  // reflection formula needs a different expansion for every order, and
  // reporting a number from a formula nothing checked would be worse.
  assert(Number.isNaN(m.polygamma(1, -1.5)), "negative non-integer is NaN, deliberately");

  // Degenerate but legal.
  near(m.gammainc(0, 1), 0, 0, "x = 0");
  near(m.betainc(0, 2, 3), 0, 0, "x = 0");
  near(m.betainc(1, 2, 3), 1, 0, "x = 1");
  near(m.ellipke(0)[0], Math.PI / 2, 1e-15, "m = 0");
  assert(!isFinite(m.ellipke(1)[0]), "K(1) diverges, as it should");
});

await run("dyna:mathx — MATLAB tier B");
