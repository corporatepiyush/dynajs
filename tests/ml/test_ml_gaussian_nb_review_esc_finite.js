// covers: dyna:ml.GaussianNB
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_gaussian_nb_review_esc_finite");
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

fam.push(["RandomForestRegressor", await new RandomForestRegressor({ nEstimators: 3, seed: 2 }).fit(Xt, yr), async (m, X) => await m.predict(X), "accept"]);
fam.push(["GradientBoostingClassifier", await new GradientBoostingClassifier({ nEstimators: 3, seed: 3 }).fit(Xt, yt), async (m, X) => await m.predict(X), "accept"]);
fam.push(["GradientBoostingRegressor", await new GradientBoostingRegressor({ nEstimators: 3, seed: 3 }).fit(Xt, yr), async (m, X) => await m.predict(X), "accept"]);
fam.push(["XGBClassifier", await new XGBClassifier({ nEstimators: 3, seed: 4 }).fit(Xt, yt), async (m, X) => await m.predict(X), "accept"]);
fam.push(["XGBRegressor", await new XGBRegressor({ nEstimators: 3, seed: 4 }).fit(Xt, yr), async (m, X) => await m.predict(X), "accept"]);
fam.push(["KMeans", await new KMeans(3, 7).fit(Xt), async (m, X) => await m.predict(X), "refuse"]);
fam.push(["GaussianNB", await new GaussianNB().fit(Xt, yt), async (m, X) => await m.predict(X), "refuse"]);
fam.push(["GaussianNB.predictProba", await new GaussianNB().fit(Xt, yt), async (m, X) => await m.predictProba(X), "refuse"]);
fam.push(["KNClassifier", await new KNClassifier(3).fit(Xt, yt), async (m, X) => await m.predict(X), "refuse"]);
fam.push(["KNRegressor", await new KNRegressor(3).fit(Xt, yr), async (m, X) => await m.predict(X), "refuse"]);
await __kitS.section("refuse", async () => {
    const m = await new SVC().fit(Xt, yt);
    fam.push(["SVC", m, async (mm, X) => await mm.predict(X), "refuse"]);
    fam.push(["SVC.decisionFunction", m, async (mm, X) => await mm.decisionFunction(X), "refuse"]);
});
__kitS.end();
const seen = new Set();
for (const [, m] of fam) if (!seen.has(m) && typeof m.close === "function") { seen.add(m); try { m.close(); } catch (e) {} }
print("review_esc_finite: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("review_esc_finite: " + fail + " failures");
