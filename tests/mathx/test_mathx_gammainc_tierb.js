// covers: dyna:mathx.gammainc
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_mathx_gammainc_tierb");
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

__kitS.section("gammainc(x=2, a=1)", () => {
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
});
__kitS.end();

print("test_mathx_tierb: all " + n + " assertions passed");
