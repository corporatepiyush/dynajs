// covers: dyna:mathx.ellipke
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_mathx_ellipke_tierb");
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

__kitS.section("K(0) = pi/2", () => {
    const [K0, E0] = m.ellipke(0);
    near(K0, Math.PI / 2, 1e-15, "K(0) = pi/2");
    near(E0, Math.PI / 2, 1e-15, "E(0) = pi/2");
    near(m.ellipke(1)[1], 1, 1e-15, "E(1) = 1");
    assert(!isFinite(m.ellipke(1)[0]), "K(1) diverges");

    for (const mm of [0.1, 0.3, 0.5, 0.9]) {
        const [K, E] = m.ellipke(mm), [Kc, Ec] = m.ellipke(1 - mm);
        near(E * Kc + Ec * K - K * Kc, Math.PI / 2, 1e-14,
            "Legendre relation at m=" + mm);
    }

    for (const mm of [0, 0.25, 0.5, 0.99, 1])
        for (const u of [-3.1, -0.4, 0, 1.3, 5.7]) {
            const { sn, cn, dn } = m.ellipj(u, mm);
            near(sn * sn + cn * cn, 1, 1e-14, "sn^2+cn^2 at " + u + "," + mm);
            near(dn * dn + mm * sn * sn, 1, 1e-14, "dn^2+m sn^2 at " + u + "," + mm);
        }
    near(m.ellipj(1.3, 0).sn, Math.sin(1.3), 1e-14, "m=0 degenerates to sin");
    near(m.ellipj(1.3, 1).sn, Math.tanh(1.3), 1e-14, "m=1 degenerates to tanh");
    throws(() => m.ellipj(1, 2), "ellipj rejects m outside [0,1]");
    throws(() => m.ellipj(1, -0.5), "ellipj rejects negative m");
});
__kitS.end();

print("test_mathx_tierb: all " + n + " assertions passed");
