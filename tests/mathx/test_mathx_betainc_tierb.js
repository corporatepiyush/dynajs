// covers: dyna:mathx.betainc
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_mathx_betainc_tierb");
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

__kitS.section("betainc(0.5,2,3) = 11/16", () => {
    near(m.betainc(0.5, 2, 3), 0.6875, 1e-14, "betainc(0.5,2,3) = 11/16");
    near(m.betainc(0.3, 1, 1), 0.3, 1e-15, "I_x(1,1) = x");
    near(m.betainc(0.3, 2, 5) + m.betainc(0.7, 5, 2), 1, 1e-14, "betainc symmetry");
    assert(m.betainc(0.5, 2, 3) !== m.betainc(0.5, 3, 2),
        "a and b are not interchangeable, so their order is tested");
    for (const [p, a, b] of [[0.6875, 2, 3], [0.1, 0.5, 4], [0.99, 7, 2]])
        near(m.betainc(m.betaincinv(p, a, b), a, b), p, 1e-10,
            "betaincinv round trip at " + p);
});
__kitS.end();

print("test_mathx_tierb: all " + n + " assertions passed");
