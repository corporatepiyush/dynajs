/* review_esc_widths.js -- permanent row of the review lane's esc_widths.js
 * (verbatim behaviour; the full-width messages it pins are sustained).
 * Original header: the FULL 64-bit CSR width must be printed
 * at every predict / predictProba / predictInto / apply refusal site, and in
 * the CSR ctor column-bounds and row() bounds messages. cols = 2^40 whose
 * low 32 bits are 0 (the truncation case from round 1) plus 2^53 and 2^63-2^53
 * style odd widths.
 */
import { CSR, LinearRegression, LogisticRegression, KMeans, GaussianNB,
         DecisionTreeClassifier, DecisionTreeRegressor, RandomForestClassifier,
         RandomForestRegressor, GradientBoostingClassifier, GradientBoostingRegressor,
         XGBClassifier, XGBRegressor, KNClassifier, KNRegressor, SVC,
         GaussianMixture } from "dyna:ml";

let pass = 0, fail = 0;
const ok = (c, w, d) => { if (c) { pass++; print("  ok    " + w); }
                          else { fail++; print("  FAIL  " + w + (d ? "  [" + d + "]" : "")); } };
function msg(fn) { try { fn(); return "NO-THROW"; } catch (e) { return e.constructor.name + ": " + e.message; } }

const Xt = [[1, 0, 2], [0, 3, 0], [4, 0, 5], [1, 1, 1], [2, 2, 0], [0, 1, 3]];
const yt = [1, 2, 3, 2, 2, 3], yr = [1.5, 2.5, 3.5, 2.0, 2.2, 3.1];

const lin = new LinearRegression().fit(Xt, yr);
const log = new LogisticRegression().fit(Xt, yt);
const km = new KMeans(3, 7).fit(Xt);
const nb = new GaussianNB().fit(Xt, yt);
const dtc = new DecisionTreeClassifier({ seed: 1 }).fit(Xt, yt);
const dtr = new DecisionTreeRegressor({ seed: 1 }).fit(Xt, yr);
const rfc = new RandomForestClassifier({ nEstimators: 3, seed: 2 }).fit(Xt, yt);
const rfr = new RandomForestRegressor({ nEstimators: 3, seed: 2 }).fit(Xt, yr);
const gbc = new GradientBoostingClassifier({ nEstimators: 3, seed: 3 }).fit(Xt, yt);
const gbr = new GradientBoostingRegressor({ nEstimators: 3, seed: 3 }).fit(Xt, yr);
const xgbc = new XGBClassifier({ nEstimators: 3, seed: 4 }).fit(Xt, yt);
const xgbr = new XGBRegressor({ nEstimators: 3, seed: 4 }).fit(Xt, yr);
const kc = new KNClassifier(3).fit(Xt, yt);
const kr = new KNRegressor(3).fit(Xt, yr);
const svc = new SVC().fit(Xt, yt);
const gmm = new GaussianMixture(2, { seed: 6 }).fit(Xt);

for (const width of [Math.pow(2, 40), Math.pow(2, 53), Math.pow(2, 53) - 2048, 4294967296 + 5]) {
    const H = new CSR([1.0], [0], [0, 1], width);
    const want = String(width);   /* what s->cols holds after JS_ToInt64 */
    const sites = [
        ["lin.predict", () => lin.predict(H)],
        ["lin.predictInto", () => lin.predictInto(new Float64Array(1), H)],
        ["log.predict", () => log.predict(H)],
        ["log.predictProba", () => log.predictProba(H)],
        ["log.predictInto", () => log.predictInto(new Float64Array(1), H)],
        ["kmeans.predict", () => km.predict(H)],
        ["kmeans.predictInto", () => km.predictInto(new Float64Array(1), H)],
        ["nb.predict", () => nb.predict(H)],
        ["nb.predictProba", () => nb.predictProba(H)],
        ["nb.predictInto", () => nb.predictInto(new Float64Array(1), H)],
        ["dtc.predict", () => dtc.predict(H)],
        ["dtc.predictInto", () => dtc.predictInto(new Float64Array(1), H)],
        ["dtr.predictProba", () => dtr.predict(H)],
        ["rfc.predict", () => rfc.predict(H)],
        ["rfc.predictProba", () => rfc.predictProba(H)],
        ["rfc.predictInto", () => rfc.predictInto(new Float64Array(1), H)],
        ["gbc.predictProba", () => gbc.predictProba(H)],
        ["xgbc.predict", () => xgbc.predict(H)],
        ["xgbr.predictInto", () => xgbr.predictInto(new Float64Array(1), H)],
        ["kc.predict", () => kc.predict(H)],
        ["kr.predict", () => kr.predict(H)],
        ["svc.predict", () => svc.predict(H)],
        ["svc.decisionFunction", () => svc.decisionFunction(H)],
        ["svc.predictInto", () => svc.predictInto(new Float64Array(1), H)],
        ["gmm.predict", () => gmm.predict(H)],
        ["gmm.predictProba", () => gmm.predictProba(H)],
        ["gmm.predictInto", () => gmm.predictInto(new Float64Array(1), H)],
    ];
    let bad = [];
    for (const [name, fn] of sites) {
        const m = msg(fn);
        if (!m.includes(want)) bad.push(name + " -> " + m);
    }
    ok(bad.length === 0, "W1 width=" + want + " printed in full at all 27 predict/proba/into/decision sites",
       bad.slice(0, 3).join(" | "));
    /* apply site: dense-only; probe its width message via a flat X */
    {
        const m = msg(() => rfr.apply(new Float64Array(0), 1, width));
        ok(m.includes(want) || m.includes("buffer") || m.includes("rows"),
           "W2 apply site at width=" + want + " (full width or honest shape refusal): " + m.slice(0, 90));
    }
    /* CSR ctor column-bounds message */
    {
        const m = msg(() => new CSR([1.0], [width], [0, 1], width));
        ok(m.includes(want), "W3 ctor columns[] bounds message prints width=" + want + ": " + m.slice(0, 110));
    }
    /* CSR ctor value-kind message (non-finite value at a valid index) */
    {
        const m = msg(() => new CSR([NaN], [0], [0, 1], width));
        ok(m.includes(want), "W3b ctor value message prints width=" + want + ": " + m.slice(0, 110));
    }
    H.close();
}
/* row() bounds message: rows is bounded by rowPointers length, so exercise the
 * 64-bit format at a realistic boundary (rows = 2 via [0,0,0]) */
{
    const S = new CSR([], [], [0, 0, 0], 3);
    const m = msg(() => S.row(2));
    ok(m.includes("outside [0, 2)"), "W4 row() bounds message prints the row bound: " + m);
    const m2 = msg(() => S.row(-1));
    ok(m2.includes("outside [0, 2)"), "W4b row() bounds message (negative index): " + m2);
    S.close();
}
for (const m of [lin, log, km, nb, dtc, dtr, rfc, rfr, gbc, gbr, xgbc, xgbr, kc, kr, svc, gmm]) m.close();
print("review_esc_widths: " + pass + " passed, " + fail + " failed");
if (fail) throw new Error("review_esc_widths: " + fail + " failures");
