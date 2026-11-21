// covers: dyna:ml.GaussianMixture
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_gaussian_mixture_review_esc_finite");
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

await __kitS.section("GaussianMixture", async () => {
    const m = await new GaussianMixture(2, { seed: 6 }).fit(Xt);
    fam.push(["GaussianMixture", m, async (mm, X) => await mm.predict(X), "refuse"]);
    fam.push(["GaussianMixture.predictProba", m, async (mm, X) => await mm.predictProba(X), "refuse"]);
});
__kitS.end();
const seen = new Set();
for (const [, m] of fam) if (!seen.has(m) && typeof m.close === "function") { seen.add(m); try { m.close(); } catch (e) {} }
print("review_esc_finite: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("review_esc_finite: " + fail + " failures");
