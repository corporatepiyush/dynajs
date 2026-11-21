// covers: dyna:mathx.legendreP
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_mathx_legendre_p_tierb");
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

__kitS.section("legendre returns the m=0..n column", () => {
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
});
__kitS.end();

print("test_mathx_tierb: all " + n + " assertions passed");
