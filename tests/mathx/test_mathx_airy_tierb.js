// covers: dyna:mathx.airy
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_mathx_airy_tierb");
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

__kitS.section("Airy Wronskian at x", () => {
    for (const x of [-30, -12, -7.5, -7, -6.9, -3, -0.5, 0, 0.05, 0.1, 0.2, 3, 15, 30]) {
        const a = m.airy(x);
        near(a.ai * a.bip - a.aip * a.bi, 1 / Math.PI, 1e-9,
            "Airy Wronskian at x=" + x);
    }
    const a0 = m.airy(0);
    near(a0.ai, Math.pow(3, -2 / 3) / m.gamma(2 / 3), 1e-14, "Ai(0)");
    near(a0.aip, -Math.pow(3, -1 / 3) / m.gamma(1 / 3), 1e-14, "Ai'(0)");
    near(a0.bi, Math.pow(3, -1 / 6) / m.gamma(2 / 3), 1e-14, "Bi(0)");
    near(a0.bip, Math.pow(3, 1 / 6) / m.gamma(1 / 3), 1e-14, "Bi'(0)");
    assert(m.airy(12).ai < 1e-12 && m.airy(12).bi > 1e10,
        "Ai decays and Bi grows on the positive axis");
    const keys = Object.keys(m.airy(1)).sort();
    assert(keys.join(",") === "ai,aip,bi,bip", "airy returns all four values");
});
__kitS.end();

print("test_mathx_tierb: all " + n + " assertions passed");
