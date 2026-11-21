// timeout: 180
// covers: dyna:ml.PCA
// Split from tests/test_data_w3_fixes.js: the ml sections.
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_pca_data_w3_fixes");
import { TOML } from "dyna:config";
import { PCA } from "dyna:ml";

let n = 0, fails = 0;
function check(c, m) { n++; if (!c) { fails++; print("FAIL: " + m); } }
function eq(a, b, m) { check(a === b, m + " -- got " + JSON.stringify(a) + ", want " + JSON.stringify(b)); }
async function throws(fn, m) { n++; try { await fn(); fails++; print("FAIL: " + m + " (did not throw)"); } catch (e) { } }
function peq(text, expected, m) {
    n++;
    let got;
    try { got = TOML.parse(text).s; }
    catch (e) { fails++; print("FAIL: " + m + " (threw " + e + ")"); return; }
    check(got === expected, m + " -- got " + JSON.stringify(got) + ", want " + JSON.stringify(expected));
}
await __kitS.section("OPT-053: PCA refuses a 1100-column Jacobi workload", async () => {
    const X = [
        Array.from({ length: 1100 }, (_, i) => i * 0.001),
        Array.from({ length: 1100 }, (_, i) => 1 - i * 0.001),
    ];
    await throws(async () => await new PCA().fit(X), "OPT-053: PCA refuses a 1100-column Jacobi workload");
    const small = await new PCA().fit([[1, 2], [3, 4], [5, 6]]);
    check(small instanceof PCA, "OPT-053: a small PCA fit still works");
});
__kitS.end();
if (fails === 0) print("test_ml_data_w3_fixes: all " + n + " checks passed");
else print("test_ml_data_w3_fixes: " + fails + " FAILED of " + n);
