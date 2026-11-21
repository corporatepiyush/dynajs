// covers: dyna:mathx.polygamma
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_mathx_polygamma_tierb");
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

__kitS.section("psi(1) = -euler", () => {
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
});
__kitS.end();

print("test_mathx_tierb: all " + n + " assertions passed");
