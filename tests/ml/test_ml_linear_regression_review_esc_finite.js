// covers: dyna:ml.LinearRegression
import { sections as __kitSections } from "../lib/kit.js";
const __kitS = __kitSections("test_ml_linear_regression_review_esc_finite");
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

await __kitS.section("F2 LinearRegression refuses a NaN row", async () => {
    const m = await new LinearRegression().fit(Xt, yr);
    ok((await outcome(async () => await m.predict([[NaN, 1, 2]]))) === "refused(RangeError)", "F2 LinearRegression refuses a NaN row");
    ok((await outcome(async () => await m.predict([[Infinity, 1, 2]]))) === "refused(RangeError)", "F2 LinearRegression refuses a +Inf row");
    m.close();
});

await __kitS.section("F3 setup: duplicate-sum overflow reads +Inf:", async () => {
    const S = new CSR([1e308, 1e308], [0, 0], [0, 2], 3);
    const D = await S.toDense();
    ok(D[0][0] === Infinity, "F3 setup: duplicate-sum overflow reads +Inf: " + D[0][0]);
    let mism = [];
    for (const [name, m, fn, want] of fam) {
        const od = await outcome(async () => await fn(m, D));
        const os = await outcome(async () => await fn(m, S));
        if (od !== os) mism.push(`${name}: dense=${od} csr=${os}`);
        if (want === "refuse" && od !== "refused(RangeError)")
            mism.push(`${name}: policy says refuse, got ${od}`);
        if (want === "accept" && od.startsWith("refused"))
            mism.push(`${name}: policy says accept, got ${od}`);
    }
    ok(mism.length === 0,
       "F3 the overflow-CLASS reading behaves identically on both forms, all families",
       mism.slice(0, 4).join(" | "));
    {
        const m = await new LinearRegression().fit(Xt, yr);
        const a = await outcome(async () => await m.predict(S)), b = await outcome(async () => await m.predict(D));
        ok(a === b && a === "refused(RangeError)", "F3b LinearRegression overflow-dup predict is refused identically by both forms: " + a + " vs " + b);
        m.close();
    }
    {
        let r = [];
        for (const [tag, build] of [["dense", async () => await new LinearRegression().fit(D, [1])],
                                    ["csr", async () => await new LinearRegression().fit(S, [1])]]) {
            try { await build(); r.push(tag + ": ACCEPTED"); }
            catch (e) { r.push(tag + ": " + e.constructor.name); }
        }
        ok(r.every(s => s.endsWith("RangeError")) && r[0].slice(-10) === r[1].slice(-10),
           "F3c LinearRegression.fit refuses the overflow reading identically both forms",
           r.join(", "));
    }
    S.close();
});
__kitS.end();
const seen = new Set();
for (const [, m] of fam) if (!seen.has(m) && typeof m.close === "function") { seen.add(m); try { m.close(); } catch (e) {} }
print("review_esc_finite: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("review_esc_finite: " + fail + " failures");
