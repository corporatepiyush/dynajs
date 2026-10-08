import { CSR, LinearRegression, LogisticRegression, KMeans, GaussianNB,
         DecisionTreeClassifier, DecisionTreeRegressor, RandomForestClassifier,
         RandomForestRegressor, GradientBoostingClassifier, GradientBoostingRegressor,
         XGBClassifier, XGBRegressor, KNClassifier, KNRegressor, SVC,
         GaussianMixture } from "dyna:ml";

let pass = 0, fail = 0;
const ok = (c, w, d) => { if (c) { pass++; print("  ok    " + w); }
                          else { fail++; print("  FAIL  " + w + (d ? "  [" + d + "]" : "")); } };
function outcome(fn) {
    try { const v = fn(); return "ACCEPTED(" + JSON.stringify(Array.from(v).flat(6).slice(0, 3)) + ")"; }
    catch (e) { return "refused(" + e.constructor.name + ")"; }
}

const Xt = [[1, 0, 2], [0, 3, 0], [4, 0, 5], [1, 1, 1], [2, 2, 0], [0, 1, 3]];
const yt = [1, 2, 3, 2, 2, 3], yr = [1.5, 2.5, 3.5, 2.0, 2.2, 3.1];
const fam = [];
fam.push(["LinearRegression", new LinearRegression().fit(Xt, yr), (m, X) => m.predict(X), "refuse"]);
fam.push(["LinearRegression.predictInto", new LinearRegression().fit(Xt, yr), (m, X) => m.predictInto(new Float64Array(1), X), "refuse"]);
{
    const m = new LogisticRegression().fit(Xt, yt);
    fam.push(["LogisticRegression", m, (mm, X) => mm.predict(X), "refuse"]);
    fam.push(["LogisticRegression.predictProba", m, (mm, X) => mm.predictProba(X), "refuse"]);
}
fam.push(["DecisionTreeClassifier", new DecisionTreeClassifier({ seed: 1 }).fit(Xt, yt), (m, X) => m.predict(X), "accept"]);
fam.push(["DecisionTreeRegressor", new DecisionTreeRegressor({ seed: 1 }).fit(Xt, yr), (m, X) => m.predict(X), "accept"]);
{
    const m = new RandomForestClassifier({ nEstimators: 3, seed: 2 }).fit(Xt, yt);
    fam.push(["RandomForestClassifier", m, (mm, X) => mm.predict(X), "accept"]);
    fam.push(["RandomForestClassifier.predictProba", m, (mm, X) => mm.predictProba(X), "accept"]);
}
fam.push(["RandomForestRegressor", new RandomForestRegressor({ nEstimators: 3, seed: 2 }).fit(Xt, yr), (m, X) => m.predict(X), "accept"]);
fam.push(["GradientBoostingClassifier", new GradientBoostingClassifier({ nEstimators: 3, seed: 3 }).fit(Xt, yt), (m, X) => m.predict(X), "accept"]);
fam.push(["GradientBoostingRegressor", new GradientBoostingRegressor({ nEstimators: 3, seed: 3 }).fit(Xt, yr), (m, X) => m.predict(X), "accept"]);
fam.push(["XGBClassifier", new XGBClassifier({ nEstimators: 3, seed: 4 }).fit(Xt, yt), (m, X) => m.predict(X), "accept"]);
fam.push(["XGBRegressor", new XGBRegressor({ nEstimators: 3, seed: 4 }).fit(Xt, yr), (m, X) => m.predict(X), "accept"]);
fam.push(["KMeans", new KMeans(3, 7).fit(Xt), (m, X) => m.predict(X), "refuse"]);
fam.push(["GaussianNB", new GaussianNB().fit(Xt, yt), (m, X) => m.predict(X), "refuse"]);
fam.push(["GaussianNB.predictProba", new GaussianNB().fit(Xt, yt), (m, X) => m.predictProba(X), "refuse"]);
fam.push(["KNClassifier", new KNClassifier(3).fit(Xt, yt), (m, X) => m.predict(X), "refuse"]);
fam.push(["KNRegressor", new KNRegressor(3).fit(Xt, yr), (m, X) => m.predict(X), "refuse"]);
{
    const m = new SVC().fit(Xt, yt);
    fam.push(["SVC", m, (mm, X) => mm.predict(X), "refuse"]);
    fam.push(["SVC.decisionFunction", m, (mm, X) => mm.decisionFunction(X), "refuse"]);
}
{
    const m = new GaussianMixture(2, { seed: 6 }).fit(Xt);
    fam.push(["GaussianMixture", m, (mm, X) => mm.predict(X), "refuse"]);
    fam.push(["GaussianMixture.predictProba", m, (mm, X) => mm.predictProba(X), "refuse"]);
}

for (const bad of [NaN, Infinity, -Infinity]) {
    const row = [bad, 1, 2];
    const D = [row];
    let lines = [], mism = [];
    for (const [name, m, fn, want] of fam) {
        const od = outcome(() => fn(m, D));
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
                                    ["fromDense", () => CSR.fromDense([row])]]) {
            try { build(); msgs.push(`${tag}: ACCEPTED`); }
            catch (e) { msgs.push(`${tag}: ${e.constructor.name}`); }
        }
        ok(msgs.every(s => s.endsWith("RangeError")),
           "F1b CSR construction refuses " + String(bad) + " at both declarations", msgs.join(", "));
    }
}

{
    const m = new LinearRegression().fit(Xt, yr);
    ok(outcome(() => m.predict([[NaN, 1, 2]])) === "refused(RangeError)", "F2 LinearRegression refuses a NaN row");
    ok(outcome(() => m.predict([[Infinity, 1, 2]])) === "refused(RangeError)", "F2 LinearRegression refuses a +Inf row");
    m.close();
}

{
    const S = new CSR([1e308, 1e308], [0, 0], [0, 2], 3);
    const D = S.toDense();
    ok(D[0][0] === Infinity, "F3 setup: duplicate-sum overflow reads +Inf: " + D[0][0]);
    let mism = [];
    for (const [name, m, fn, want] of fam) {
        const od = outcome(() => fn(m, D));
        const os = outcome(() => fn(m, S));
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
        const m = new LinearRegression().fit(Xt, yr);
        const a = outcome(() => m.predict(S)), b = outcome(() => m.predict(D));
        ok(a === b && a === "refused(RangeError)", "F3b LinearRegression overflow-dup predict is refused identically by both forms: " + a + " vs " + b);
        m.close();
    }
    {
        let r = [];
        for (const [tag, build] of [["dense", () => new LinearRegression().fit(D, [1])],
                                    ["csr", () => new LinearRegression().fit(S, [1])]]) {
            try { build(); r.push(tag + ": ACCEPTED"); }
            catch (e) { r.push(tag + ": " + e.constructor.name); }
        }
        ok(r.every(s => s.endsWith("RangeError")) && r[0].slice(-10) === r[1].slice(-10),
           "F3c LinearRegression.fit refuses the overflow reading identically both forms",
           r.join(", "));
    }
    S.close();
}
const seen = new Set();
for (const [, m] of fam) if (!seen.has(m) && typeof m.close === "function") { seen.add(m); try { m.close(); } catch (e) {} }
print("review_esc_finite: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("review_esc_finite: " + fail + " failures");
