// covers: dyna:ml.CSR
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_csr_review_esc_finite");
import { CSR, LinearRegression, LogisticRegression, KMeans, GaussianNB,
         DecisionTreeClassifier, DecisionTreeRegressor, RandomForestClassifier,
         RandomForestRegressor, GradientBoostingClassifier, GradientBoostingRegressor,
         XGBClassifier, XGBRegressor, KNClassifier, KNRegressor, SVC,
         GaussianMixture } from "dyna:ml";

let pass = 0, fail = 0;
const ok = (c, w, d) => { if (c) { pass++; print("  ok    " + w); }
                          else { fail++; print("  FAIL  " + w + (d ? "  [" + d + "]" : "")); } };
async function outcome(fn) {
    try { const v = await fn(); return "ACCEPTED(" + JSON.stringify(Array.from(v).flat(6).slice(0, 3)) + ")"; }
    catch (e) { return "refused(" + e.constructor.name + ")"; }
}

const Xt = [[1, 0, 2], [0, 3, 0], [4, 0, 5], [1, 1, 1], [2, 2, 0], [0, 1, 3]];
const yt = [1, 2, 3, 2, 2, 3], yr = [1.5, 2.5, 3.5, 2.0, 2.2, 3.1];
const fam = [];
fam.push(["LinearRegression", await new LinearRegression().fit(Xt, yr), async (m, X) => await m.predict(X), "refuse"]);
fam.push(["LinearRegression.predictInto", await new LinearRegression().fit(Xt, yr), async (m, X) => await m.predictInto(new Float64Array(1), X), "refuse"]);

await __kitS.section("INFO finite map for", async () => {
for (const bad of [NaN, Infinity, -Infinity]) {
    const row = [bad, 1, 2];
    const D = [row];
    let lines = [], mism = [];
    for (const [name, m, fn, want] of fam) {
        const od = await outcome(async () => await fn(m, D));
        lines.push(`${name}=${od.startsWith("refused") ? "refused" : "ACCEPTED"}`);
        const dRef = od.startsWith("refused");
        if (want === "refuse" && !dRef) mism.push(`${name} should refuse (doc) but: ${od}`);
        if (want === "refuse" && dRef && !od.includes("RangeError")) mism.push(`${name} wrong error class: ${od}`);
        if (want === "accept" && dRef) mism.push(`${name} should accept silently (doc) but: ${od}`);
    }
    print("INFO  finite map for " + String(bad) + " (dense): " + lines.join(", "));
    ok(mism.length === 0, "F1 policy + doc match for " + String(bad) + " (dense form)",
       mism.slice(0, 3).join(" | "));
    {
        let msgs = [];
        for (const [tag, build] of [["ctor", () => new CSR([bad, 1, 2], [0, 1, 2], [0, 3], 3)],
                                    ["fromDense", async () => await CSR.fromDense([row])]]) {
            try { await build(); msgs.push(`${tag}: ACCEPTED`); }
            catch (e) { msgs.push(`${tag}: ${e.constructor.name}`); }
        }
        ok(msgs.every(s => s.endsWith("RangeError")),
           "F1b CSR construction refuses " + String(bad) + " at both declarations", msgs.join(", "));
    }
}
const seen = new Set();
for (const [, m] of fam) if (!seen.has(m) && typeof m.close === "function") { seen.add(m); try { m.close(); } catch (e) {} }
print("review_esc_finite: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("review_esc_finite: " + fail + " failures");
});
__kitS.end();
