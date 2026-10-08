import { CSR, LinearRegression, LogisticRegression, KMeans, GaussianNB,
         DecisionTreeClassifier, DecisionTreeRegressor, RandomForestClassifier,
         RandomForestRegressor, GradientBoostingClassifier, GradientBoostingRegressor,
         XGBClassifier, XGBRegressor, KNClassifier, KNRegressor, SVC,
         GaussianMixture } from "dyna:ml";

let pass = 0, fail = 0;
const ok = (c, w, d) => { if (c) { pass++; print("  ok    " + w); }
                          else { fail++; print("  FAIL  " + w + (d ? "  [" + d + "]" : "")); } };
async function msg(fn) { try { await fn(); return "NO-THROW"; } catch (e) { return e.constructor.name + ": " + e.message; } }

const Xt = [[1, 0, 2], [0, 3, 0], [4, 0, 5], [1, 1, 1], [2, 2, 0], [0, 1, 3]];
const yt = [1, 2, 3, 2, 2, 3], yr = [1.5, 2.5, 3.5, 2.0, 2.2, 3.1];

const lin = await new LinearRegression().fit(Xt, yr);
const log = await new LogisticRegression().fit(Xt, yt);
const km = await new KMeans(3, 7).fit(Xt);
const nb = await new GaussianNB().fit(Xt, yt);
const dtc = await new DecisionTreeClassifier({ seed: 1 }).fit(Xt, yt);
const dtr = await new DecisionTreeRegressor({ seed: 1 }).fit(Xt, yr);
const rfc = await new RandomForestClassifier({ nEstimators: 3, seed: 2 }).fit(Xt, yt);
const rfr = await new RandomForestRegressor({ nEstimators: 3, seed: 2 }).fit(Xt, yr);
const gbc = await new GradientBoostingClassifier({ nEstimators: 3, seed: 3 }).fit(Xt, yt);
const gbr = await new GradientBoostingRegressor({ nEstimators: 3, seed: 3 }).fit(Xt, yr);
const xgbc = await new XGBClassifier({ nEstimators: 3, seed: 4 }).fit(Xt, yt);
const xgbr = await new XGBRegressor({ nEstimators: 3, seed: 4 }).fit(Xt, yr);
const kc = await new KNClassifier(3).fit(Xt, yt);
const kr = await new KNRegressor(3).fit(Xt, yr);
const svc = await new SVC().fit(Xt, yt);
const gmm = await new GaussianMixture(2, { seed: 6 }).fit(Xt);

for (const width of [Math.pow(2, 40), Math.pow(2, 53), Math.pow(2, 53) - 2048, 4294967296 + 5]) {
    const H = new CSR([1.0], [0], [0, 1], width);
    const want = String(width);
    const sites = [
        ["lin.predict", async () => await lin.predict(H)],
        ["lin.predictInto", async () => await lin.predictInto(new Float64Array(1), H)],
        ["log.predict", async () => await log.predict(H)],
        ["log.predictProba", async () => await log.predictProba(H)],
        ["log.predictInto", async () => await log.predictInto(new Float64Array(1), H)],
        ["kmeans.predict", async () => await km.predict(H)],
        ["kmeans.predictInto", async () => await km.predictInto(new Float64Array(1), H)],
        ["nb.predict", async () => await nb.predict(H)],
        ["nb.predictProba", async () => await nb.predictProba(H)],
        ["nb.predictInto", async () => await nb.predictInto(new Float64Array(1), H)],
        ["dtc.predict", async () => await dtc.predict(H)],
        ["dtc.predictInto", async () => await dtc.predictInto(new Float64Array(1), H)],
        ["dtr.predictProba", async () => await dtr.predict(H)],
        ["rfc.predict", async () => await rfc.predict(H)],
        ["rfc.predictProba", async () => await rfc.predictProba(H)],
        ["rfc.predictInto", async () => await rfc.predictInto(new Float64Array(1), H)],
        ["gbc.predictProba", async () => await gbc.predictProba(H)],
        ["xgbc.predict", async () => await xgbc.predict(H)],
        ["xgbr.predictInto", async () => await xgbr.predictInto(new Float64Array(1), H)],
        ["kc.predict", async () => await kc.predict(H)],
        ["kr.predict", async () => await kr.predict(H)],
        ["svc.predict", async () => await svc.predict(H)],
        ["svc.decisionFunction", async () => await svc.decisionFunction(H)],
        ["svc.predictInto", async () => await svc.predictInto(new Float64Array(1), H)],
        ["gmm.predict", async () => await gmm.predict(H)],
        ["gmm.predictProba", async () => await gmm.predictProba(H)],
        ["gmm.predictInto", async () => await gmm.predictInto(new Float64Array(1), H)],
    ];
    let bad = [];
    for (const [name, fn] of sites) {
        const m = await msg(fn);
        if (!m.includes(want)) bad.push(name + " -> " + m);
    }
    ok(bad.length === 0, "W1 width=" + want + " printed in full at all 27 predict/proba/into/decision sites",
       bad.slice(0, 3).join(" | "));
    {
        const m = await msg(async () => await rfr.apply(new Float64Array(0), 1, width));
        ok(m.includes(want) || m.includes("buffer") || m.includes("rows"),
           "W2 apply site at width=" + want + " (full width or honest shape refusal): " + m.slice(0, 90));
    }
    {
        const m = await msg(() => new CSR([1.0], [width], [0, 1], width));
        ok(m.includes(want), "W3 ctor columns[] bounds message prints width=" + want + ": " + m.slice(0, 110));
    }
    {
        const m = await msg(() => new CSR([NaN], [0], [0, 1], width));
        ok(m.includes(want), "W3b ctor value message prints width=" + want + ": " + m.slice(0, 110));
    }
    H.close();
}
{
    const S = new CSR([], [], [0, 0, 0], 3);
    const m = await msg(() => S.row(2));
    ok(m.includes("outside [0, 2)"), "W4 row() bounds message prints the row bound: " + m);
    const m2 = await msg(() => S.row(-1));
    ok(m2.includes("outside [0, 2)"), "W4b row() bounds message (negative index): " + m2);
    S.close();
}
for (const m of [lin, log, km, nb, dtc, dtr, rfc, rfr, gbc, gbr, xgbc, xgbr, kc, kr, svc, gmm]) m.close();
print("review_esc_widths: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("review_esc_widths: " + fail + " failures");
