// flags: --std
import { test, run, assert } from "./harness.js";
import * as m from "dyna:mathx";

const near = (a, b, tol, msg) => {
  if (!(Math.abs(a - b) <= tol * Math.max(1, Math.abs(b))))
    throw new Error(`${msg}: ${a} vs ${b}`);
};

test("identities hold, and that is how these are checked", () => {
  near(m.gammainc(2, 1), 1 - Math.exp(-2), 1e-14, "gammainc vs its closed form");
  near(m.betainc(0.5, 2, 3), 0.6875, 1e-14, "betainc vs the binomial tail");
  const [K, E] = m.ellipke(0.3), [Kc, Ec] = m.ellipke(0.7);
  near(E * Kc + Ec * K - K * Kc, Math.PI / 2, 1e-14, "Legendre's relation");
});

test("BEST: the Wronskian holds to 1e-13 across both Bessel seams", () => {
  let worst = 0;
  for (const x of [0.05, 0.4, 0.6, 3.7, 19.5, 21, 200]) {
    const w = m.besseli(0, x) * m.besselk(1, x) + m.besseli(1, x) * m.besselk(0, x);
    worst = Math.max(worst, Math.abs(w - 1 / x) * x);
  }
  print(`  Bessel Wronskian worst relative error: ${worst.toExponential(2)}`);
  assert(worst < 1e-13, "across the x=0.5 and x=20 switch points");
});

test("BEST vs WORST: the Airy Wronskian, by regime", () => {
  const band = { "x>=0.1 (Bessel)": 0, "-7..0.1 (series)": 0, "x<-7 (asymptotic)": 0 };
  for (let x = -30; x <= 30; x += 0.05) {
    const a = m.airy(x);
    const e = Math.abs((a.ai * a.bip - a.aip * a.bi) * Math.PI - 1);
    const k = x >= 0.1 ? "x>=0.1 (Bessel)" : x >= -7 ? "-7..0.1 (series)" : "x<-7 (asymptotic)";
    band[k] = Math.max(band[k], e);
  }
  for (const k of Object.keys(band)) print(`  ${k.padEnd(20)} ${band[k].toExponential(2)}`);
  assert(band["-7..0.1 (series)"] > band["x<-7 (asymptotic)"],
    "the Maclaurin series, not the transition band, is the weak regime");
  assert(band["-7..0.1 (series)"] < 1e-10, "and it is still good to ten digits");
});

test("the scaled forms exist because the raw pair cannot survive", () => {
  assert(m.besselk(0, 800) === 0, "K underflows");
  assert(!isFinite(m.besseli(0, 800)), "I overflows");
  const w = m.besseliScaled(0, 800) * m.besselkScaled(1, 800) +
            m.besseliScaled(1, 800) * m.besselkScaled(0, 800);
  near(w, 1 / 800, 1e-12, "the scaled pair still holds where the raw one cannot");
});

test("MATLAB's spellings, including the ones that differ", () => {
  assert(m.gammainc(2, 1) !== m.gammainc(1, 2), "the argument order is real");
  assert(m.legendre(3, 0.4).length === 4, "the whole column");
  near(m.legendreP(1, 1, 0.4), -Math.sqrt(1 - 0.16), 1e-15, "Condon-Shortley phase");
  print(`  idivide(-7,2): fix ${m.idivide(-7, 2)}  floor ${m.idivide(-7, 2, "floor")}` +
        `  ceil ${m.idivide(-7, 2, "ceil")}  round ${m.idivide(-7, 2, "round")}`);
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

  assert(Number.isNaN(m.legendreP(2, 3, 0.4)), "order above degree");
  assert(Number.isNaN(m.legendreP(2, 0, 1.5)), "|x| > 1");
  assert(Number.isNaN(m.polygamma(1, -2)), "a pole");
  assert(Number.isNaN(m.polygamma(1, -1.5)), "negative non-integer is NaN, deliberately");

  near(m.gammainc(0, 1), 0, 0, "x = 0");
  near(m.betainc(0, 2, 3), 0, 0, "x = 0");
  near(m.betainc(1, 2, 3), 1, 0, "x = 1");
  near(m.ellipke(0)[0], Math.PI / 2, 1e-15, "m = 0");
  assert(!isFinite(m.ellipke(1)[0]), "K(1) diverges, as it should");
});

await run("dyna:mathx — MATLAB tier B");
