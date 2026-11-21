// covers: dyna:ml.StandardScaler
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_standard_scaler_decomposition");
import { PCA, GaussianNB, StandardScaler } from "dyna:ml";

let n = 0;
function assert(cond, msg) { n++; if (!cond) throw new Error("assertion failed: " + msg); }
function approx(a, b, eps, msg) {
    n++;
    if (!(Math.abs(a - b) <= eps)) throw new Error(`approx failed: ${msg} (${a} vs ${b})`);
}
async function throws(fn, msg, ErrType) {
    n++;
    try { await fn(); } catch (e) {
        if (ErrType && !(e instanceof ErrType))
            throw new Error(`${msg}: wrong error type ${e.constructor.name}: ${e.message}`);
        return;
    }
    throw new Error("expected a throw: " + msg);
}

await __kitS.section("pipeline classifies unseen points", async () => {
    const X = [[100, 0.1], [110, 0.12], [90, 0.09], [500, 0.9], [520, 0.95], [480, 0.85]];
    const y = [0, 0, 0, 1, 1, 1];
    const sc = new StandardScaler(), p = new PCA(1), nb = new GaussianNB();
    try {
        const z = await sc.fitTransform(X);
        const t = await p.fitTransform(z);
        await nb.fit(t, y);
        const pred = await nb.predict(t);
        for (let i = 0; i < 6; i++)
            assert(pred[i] === y[i], `pipeline classifies training point ${i}`);
        const q = await p.transform(await sc.transform([[105, 0.11], [510, 0.9]]));
        const qp = await nb.predict(q);
        assert(qp[0] === 0 && qp[1] === 1, "pipeline classifies unseen points");
    } finally { sc.close(); p.close(); nb.close(); }
});
__kitS.end();

print("test_ml_decomposition: all tests passed (" + n + " assertions)");
