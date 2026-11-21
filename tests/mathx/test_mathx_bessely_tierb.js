// covers: dyna:mathx.bessely
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_mathx_bessely_tierb");
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

__kitS.section("Wronskian at x", () => {
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
});
__kitS.end();

print("test_mathx_tierb: all " + n + " assertions passed");
