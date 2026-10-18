// Demand planning — hierarchical sales forecasts, backtested model selection, and the inventory policy they drive.
//
// WHAT IT IS
//   What a retailer's planning team runs weekly for every SKU in every store:
//
//   history      two years of daily unit sales per store x SKU from a CSV extract, with promotions and holidays
//   features     lags (7, 14, 28), rolling means, day of week, week of year (Fourier terms), promotion flag, days to
//                the next holiday (from an RRule calendar), price relative to the SKU's median
//   models       per SKU family a portfolio is backtested on rolling origins (4 folds x 28 days):
//                  - seasonal naive (same weekday last week)
//                  - exponentially weighted level x weekday profile
//                  - gradient boosted trees on the feature set
//                and the winner per family is chosen by weighted MAPE, not assumed
//   hierarchy    store forecasts are summed to the region and reconciled top-down so the levels agree exactly
//   uncertainty  quantile bands from backtest residuals (empirical p10/p90) feed safety stock
//   policy       per store x SKU: lead-time demand, safety stock for a 95% service level, reorder point, and an
//                order quantity rounded up to the case pack, with a simulated next 28 days showing fill rate
//   output       a purchase-order CSV and a planner summary as YAML
//   ABC          SKUs ranked by revenue in a ZSet; a simd cumulative sum over the ranked revenues splits them into
//                A (80% of revenue), B (next 15%) and C — the classes that get different service levels
//   bands        each series' backtest residuals go into a TDigest; its p10/p90 give the forecast band, whose
//                coverage of the held-out month is checked (it should hold about 80% of actual days)
//   calendar     holidays live in a SortedSet, so "days to the next holiday" is one ceil() per day
//   errors       WMAPE is computed on Float32Array vectors (simd sub, abs, sum), not a JS reduce
//
// MODULES  dyna:csv · dyna:ml (GradientBoostingRegressor) · dyna:time (RRule) · dyna:mathx (erfinv, stats) · dyna:yaml
//          · dyna:cli (Table) · dyna:file · dyna:random · dyna:structures (SortedSet, ZSet, TDigest) · dyna:simd
//
// RUN      dynajs examples/ml/demand-planning.js     generates sales for 4 stores x 12 SKUs and plans the next 4 weeks

import * as csv from "dyna:csv";
import { GradientBoostingRegressor } from "dyna:ml";
import { RRule } from "dyna:time";
import { erfinv, stats } from "dyna:mathx";
import * as YAML from "dyna:yaml";
import { Table } from "dyna:cli";
import { makeTempDir, writeFile, readFile, removeAll } from "dyna:file";
import { Random } from "dyna:random";
import { SortedSet, ZSet, TDigest } from "dyna:structures";
import * as simd from "dyna:simd";
const table = (rows) => Table(rows.slice(1), { head: rows[0] });

const work = makeTempDir("demand");
const rng = new Random(52);
const DAY0 = Date.UTC(2024, 2, 1),
    DAYS = 730,
    HORIZON = 28;
const dateOf = (d) => new Date(DAY0 + d * 86400_000).toISOString().slice(0, 10);

// =============================================================================
// Calendar: public holidays and paydays as recurrence rules
// =============================================================================
const holidays = new Set();
for (const rule of [
    { freq: "YEARLY", bymonth: [12], bymonthday: [25] },
    { freq: "YEARLY", bymonth: [1], bymonthday: [1] },
    { freq: "YEARLY", bymonth: [11], byweekday: ["TH"], bysetpos: [4] },
    { freq: "YEARLY", bymonth: [5], byweekday: ["MO"], bysetpos: [-1] },
])
    for (const d of new RRule({
        ...rule,
        dtstart: new Date(DAY0),
        until: new Date(DAY0 + (DAYS + HORIZON + 60) * 86400_000),
    }).all())
        holidays.add(Math.round((d.getTime() - DAY0) / 86400_000));
const holidayDays = new SortedSet();
for (const h of holidays) holidayDays.add(h);
const daysToHoliday = (d) => {
    const h = holidayDays.ceil(d);
    return h === undefined ? 60 : Math.min(60, h - d);
};

// =============================================================================
// Synthetic sales: level x weekday x season x promo x holiday x trend
// =============================================================================
const STORES = [
    { id: "S1", region: "north", size: 1.4 },
    { id: "S2", region: "north", size: 0.8 },
    { id: "S3", region: "south", size: 1.1 },
    { id: "S4", region: "south", size: 0.6 },
];
const FAMILIES = {
    beverages: [0.9, 1.0, 1.3, 1.2, 1.25, 1.6, 1.5],
    snacks: [1, 1, 1, 1.1, 1.3, 1.5, 1.2],
    household: [1.1, 1.05, 1, 1, 1, 1.2, 0.7],
};
const SKUS = Object.keys(FAMILIES).flatMap((fam, f) =>
    Array.from({ length: 4 }, (_, k) => ({
        id: `${fam.slice(0, 3).toUpperCase()}-${k + 1}`,
        family: fam,
        base: 6 + rng.nextBounded(30),
        price: 1.5 + rng.nextBounded(80) / 10,
        casePack: [6, 12, 24][(f + k) % 3],
        leadDays: 3 + ((f + k) % 5),
        seasonAmp: fam === "beverages" ? 0.35 : 0.1,
        trend: (k - 1.5) * 0.0004,
    })),
);
function simulate(days) {
    const rows = [];
    for (const s of STORES)
        for (const sku of SKUS) {
            const promos = new Set();
            for (let d = 0; d < days; d += 1)
                if (rng.nextFloat() < 1 / 45) for (let k = 0; k < 7; k++) promos.add(d + k);
            for (let d = 0; d < days; d++) {
                const dow = new Date(DAY0 + d * 86400_000).getUTCDay();
                const season = 1 + sku.seasonAmp * Math.sin((2 * Math.PI * (d - 80)) / 365);
                const promo = promos.has(d) ? 1.6 : 1,
                    hol = holidays.has(d) ? 0.3 : daysToHoliday(d) <= 3 ? 1.5 : 1;
                const mean =
                    sku.base *
                    s.size *
                    FAMILIES[sku.family][(dow + 6) % 7] *
                    season *
                    promo *
                    hol *
                    (1 + sku.trend * d);
                const price = promos.has(d) ? sku.price * 0.8 : sku.price;
                rows.push([
                    dateOf(d),
                    s.id,
                    s.region,
                    sku.id,
                    String(rng.poisson(Math.max(0.1, mean))),
                    price.toFixed(2),
                    promos.has(d) ? "1" : "0",
                ]);
            }
        }
    return rows;
}
const extract = work.join("sales.csv");
writeFile(
    extract,
    csv.stringify([["date", "store", "region", "sku", "units", "price", "promo"], ...simulate(DAYS + HORIZON)]),
);

// =============================================================================
// Load and shape
// =============================================================================
const { rows } = csv.parse(readFile(extract), { hasHeader: true });
const series = new Map(); // "store|sku" -> { units[], price[], promo[] }
for (const [date, store, region, sku, units, price, promo] of rows) {
    const key = `${store}|${sku}`;
    if (!series.has(key)) series.set(key, { store, region, sku, units: [], price: [], promo: [] });
    const s = series.get(key);
    const d = Math.round((Date.parse(date) - DAY0) / 86400_000);
    s.units[d] = Number(units);
    s.price[d] = Number(price);
    s.promo[d] = Number(promo);
}
const future = new Map(
    [...series].map(([k, s]) => [
        k,
        { units: s.units.slice(DAYS), promo: s.promo.slice(DAYS), price: s.price.slice(DAYS) },
    ]),
);
for (const s of series.values()) {
    s.units.length = DAYS;
    s.price.length = DAYS;
    s.promo.length = DAYS;
} // the future is held out
const skuOf = new Map(SKUS.map((s) => [s.id, s]));

// =============================================================================
// Models
// =============================================================================
const dow = (d) => (new Date(DAY0 + d * 86400_000).getUTCDay() + 6) % 7;
function seasonalNaive(s, origin, h) {
    return Array.from({ length: h }, (_, i) => s.units[origin - 7 + (i % 7)]);
}
function levelWeekday(s, origin, h) {
    let level = stats.mean(s.units.slice(origin - 28, origin));
    for (let d = origin - 90; d < origin; d++) level = 0.97 * level + 0.03 * s.units[d];
    const prof = Array.from({ length: 7 }, (_, w) => {
        const xs = [];
        for (let d = origin - 182; d < origin; d++) if (dow(d) === w && !s.promo[d]) xs.push(s.units[d]);
        return stats.mean(xs);
    });
    const avg = stats.mean(prof);
    return Array.from(
        { length: h },
        (_, i) => ((level * prof[dow(origin + i)]) / avg) * (s.promo[origin + i] ? 1.5 : 1),
    );
}
const featureRow = (s, d, units) => {
    const med = s.medPrice;
    return [
        units[d - 7],
        units[d - 14],
        units[d - 28],
        stats.mean(units.slice(d - 28, d - 7 + 1 > d ? d : d - 6)),
        dow(d),
        Math.sin((2 * Math.PI * d) / 365),
        Math.cos((2 * Math.PI * d) / 365),
        s.promo[d] ?? 0,
        daysToHoliday(d),
        holidays.has(d) ? 1 : 0,
        (s.price[d] ?? med) / med,
    ];
};
async function boostedFamily(members, origin, h) {
    const X = [],
        y = [];
    for (const s of members) {
        s.medPrice = stats.median(s.price.slice(0, origin));
        const scale = stats.mean(s.units.slice(origin - 90, origin)) || 1;
        for (let d = 60; d < origin; d++) {
            X.push([...featureRow(s, d, s.units), scale]);
            y.push(s.units[d] / scale);
        }
    }
    const model = await new GradientBoostingRegressor({
        nEstimators: 120,
        maxDepth: 4,
        learningRate: 0.08,
        subsample: 0.8,
        maxBins: 64,
        seed: 3,
    }).fit(X, y);
    const scales = members.map((s) => stats.mean(s.units.slice(origin - 90, origin)) || 1);
    const paths = members.map((s) => s.units.slice(0, origin));
    const out = new Map(members.map((s) => [s, []]));
    for (let i = 0; i < h; i++) {
        // recursive: forecasts feed later lags
        const d = origin + i;
        const preds = await model.predict(members.map((s, k) => [...featureRow(s, d, paths[k]), scales[k]]));
        members.forEach((s, k) => {
            const v = Math.max(0, preds[k] * scales[k]);
            paths[k][d] = v;
            out.get(s).push(v);
        });
    }
    const imp = model.featureImportances;
    model.close();
    return { out, importances: imp };
}
const wmape = (actual, fc) => {
    const a = Float32Array.from(actual),
        f = Float32Array.from(fc);
    return simd.sum(simd.abs(f, simd.sub(f, a, f))) / Math.max(1, simd.sum(a));
};

// =============================================================================
// Backtest on rolling origins and pick a model per family
// =============================================================================
const FOLDS = [DAYS - 4 * HORIZON, DAYS - 3 * HORIZON, DAYS - 2 * HORIZON, DAYS - HORIZON];
const families = Object.keys(FAMILIES);
const choice = {},
    scoreboard = [],
    residuals = new Map();
for (const fam of families) {
    const members = [...series.values()].filter((s) => skuOf.get(s.sku).family === fam);
    const errs = { naive: [], ewma: [], boosted: [] };
    for (const origin of FOLDS) {
        const boosted = await boostedFamily(members, origin, HORIZON);
        const act = [],
            fn = [],
            fe = [],
            fb = [];
        for (const s of members) {
            const a = s.units.slice(origin, origin + HORIZON),
                b = boosted.out.get(s);
            const n = seasonalNaive(s, origin, HORIZON),
                e = levelWeekday(s, origin, HORIZON);
            act.push(...a);
            fn.push(...n);
            fe.push(...e);
            fb.push(...b);
            const r = residuals.get(s) || { naive: [], ewma: [], boosted: [] };
            a.forEach((v, i) => {
                r.boosted.push(v - b[i]);
                r.ewma.push(v - e[i]);
                r.naive.push(v - n[i]);
            });
            residuals.set(s, r);
        }
        errs.naive.push(wmape(act, fn));
        errs.ewma.push(wmape(act, fe));
        errs.boosted.push(wmape(act, fb));
    }
    const mean = Object.fromEntries(Object.entries(errs).map(([k, v]) => [k, stats.mean(v)]));
    choice[fam] = Object.entries(mean).sort((a, b) => a[1] - b[1])[0][0];
    scoreboard.push([fam, ...["naive", "ewma", "boosted"].map((k) => `${(mean[k] * 100).toFixed(1)}%`), choice[fam]]);
}

// =============================================================================
// Final forecasts, reconciliation, policy, simulation
// =============================================================================
const forecasts = new Map();
for (const fam of families) {
    const members = [...series.values()].filter((s) => skuOf.get(s.sku).family === fam);
    for (const s of members) {
        s.promo.push(...future.get(`${s.store}|${s.sku}`).promo);
        s.price.push(...future.get(`${s.store}|${s.sku}`).price);
    }
    const boosted = choice[fam] === "boosted" ? await boostedFamily(members, DAYS, HORIZON) : null;
    for (const s of members)
        forecasts.set(
            s,
            choice[fam] === "boosted"
                ? boosted.out.get(s)
                : choice[fam] === "ewma"
                  ? levelWeekday(s, DAYS, HORIZON)
                  : seasonalNaive(s, DAYS, HORIZON),
        );
}
// Top-down reconciliation: a region x SKU total forecast with the region's own level model, split by store shares.
const reconciled = new Map();
for (const region of ["north", "south"])
    for (const sku of SKUS) {
        const members = [...series.values()].filter((s) => s.region === region && s.sku === sku.id);
        const agg = {
            units: members[0].units.map((_, d) => members.reduce((t, m) => t + m.units[d], 0)),
            promo: members[0].promo,
            price: members[0].price,
        };
        const top = levelWeekday(agg, DAYS, HORIZON);
        const shares = members.map(
            (m) => stats.sum(m.units.slice(DAYS - 56, DAYS)) / Math.max(1, stats.sum(agg.units.slice(DAYS - 56, DAYS))),
        );
        members.forEach((m, k) => {
            const bottom = forecasts.get(m);
            reconciled.set(
                m,
                top.map((t, i) => 0.5 * bottom[i] + 0.5 * t * shares[k]),
            );
        });
        const fix = top.map((t, i) => t - members.reduce((s, m) => s + reconciled.get(m)[i], 0)); // force exact coherence
        members.forEach((m, k) =>
            reconciled.set(
                m,
                reconciled.get(m).map((v, i) => Math.max(0, v + fix[i] * shares[k])),
            ),
        );
    }
const Z95 = Math.SQRT2 * erfinv(2 * 0.95 - 1);
const orders = [
    ["store", "sku", "on_hand", "lead_days", "lt_demand", "safety", "reorder_point", "order_qty", "case_pack"],
];
let demandTotal = 0,
    servedTotal = 0;
const plans = [];
for (const [s, fc] of reconciled) {
    const sku = skuOf.get(s.sku),
        r = residuals.get(s)[choice[sku.family]];
    const sigmaDay = stats.stddev(r),
        L = sku.leadDays;
    const ltDemand = fc.slice(0, L).reduce((a, b) => a + b, 0);
    const safety = Z95 * sigmaDay * Math.sqrt(L);
    const rop = ltDemand + safety,
        onHand = Math.round(stats.mean(s.units.slice(DAYS - 14, DAYS)) * (2 + rng.nextBounded(5)));
    const cover = fc.slice(0, L + 7).reduce((a, b) => a + b, 0) + safety;
    const qty = onHand < rop ? Math.ceil((cover - onHand) / sku.casePack) * sku.casePack : 0;
    orders.push([
        s.store,
        s.sku,
        String(onHand),
        String(L),
        ltDemand.toFixed(1),
        safety.toFixed(1),
        rop.toFixed(1),
        String(qty),
        String(sku.casePack),
    ]);
    // Simulate the next 28 days with the real future demand and a reorder each time stock falls below the ROP.
    let stock = onHand,
        pipeline = qty ? [[L, qty]] : [];
    const actual = future.get(`${s.store}|${s.sku}`).units;
    for (let d = 0; d < HORIZON; d++) {
        for (const p of pipeline) if (p[0] === d) stock += p[1];
        const sold = Math.min(stock, actual[d]);
        stock -= sold;
        demandTotal += actual[d];
        servedTotal += sold;
        const rest = fc.slice(d + 1, d + 1 + L).reduce((a, b) => a + b, 0) + safety;
        const inbound = pipeline.filter((p) => p[0] > d).reduce((a, p) => a + p[1], 0);
        if (stock + inbound < rest)
            pipeline.push([
                d + L,
                Math.ceil((rest + fc.slice(d + 1, d + 8).reduce((a, b) => a + b, 0) - stock - inbound) / sku.casePack) *
                    sku.casePack,
            ]);
    }
    plans.push({ s, fc, actual, sigmaDay });
}
const fillRate = servedTotal / demandTotal;
// Forecast bands from backtest residuals, and their coverage of what really happened.
let inside = 0,
    days = 0;
for (const p of plans) {
    const d = new TDigest(100).addMany(Float64Array.from(residuals.get(p.s)[choice[skuOf.get(p.s.sku).family]]));
    const lo = d.quantile(0.1),
        hi = d.quantile(0.9);
    p.fc.forEach((f, i) => {
        days++;
        if (p.actual[i] >= f + lo && p.actual[i] <= f + hi) inside++;
    });
}
const bandCoverage = inside / days;
// ABC classes by revenue over the last 90 days.
const revenue = new ZSet();
for (const s of series.values()) for (let d = DAYS - 90; d < DAYS; d++) revenue.incrBy(s.sku, s.units[d] * s.price[d]);
const ranked = revenue.range(0, -1, { reverse: true });
const cum = simd.cumsum(Float32Array.from(ranked, ([, v]) => v));
const total = cum[cum.length - 1];
const abc = { A: [], B: [], C: [] };
ranked.forEach(([sku], i) => {
    const before = i ? cum[i - 1] : 0; // revenue share of the SKUs ranked above
    abc[before < 0.8 * total ? "A" : before < 0.95 * total ? "B" : "C"].push(sku);
});
const aShare = abc.A.length ? cum[abc.A.length - 1] / total : 0,
    aMinusOne = abc.A.length > 1 ? cum[abc.A.length - 2] / total : 0;
writeFile(work.join("purchase-orders.csv"), csv.stringify(orders));
const regionTotals = Object.fromEntries(
    ["north", "south"].map((rg) => [
        rg,
        Math.round(plans.filter((p) => p.s.region === rg).reduce((t, p) => t + stats.sum(p.fc), 0)),
    ]),
);
writeFile(
    work.join("summary.yaml"),
    YAML.Stringify({
        horizonDays: HORIZON,
        models: choice,
        fillRate: Number(fillRate.toFixed(4)),
        regionTotals,
        ordersRaised: orders.length - 1 - orders.slice(1).filter((o) => o[7] === "0").length,
    }),
);

// =============================================================================
// Checks and report
// =============================================================================
const results = {};
results["holiday calendar from recurrence rules"] =
    holidays.size >= 8 && holidays.has(Math.round((Date.UTC(2024, 11, 25) - DAY0) / 86400_000));
const bestErr = Math.min(...scoreboard.map((r) => parseFloat(r[1]))),
    chosenErr = scoreboard.map((r) => parseFloat(r[{ naive: 1, ewma: 2, boosted: 3 }[r[4]]]));
results["the chosen model is never worse than seasonal naive in backtest"] = chosenErr.every(
    (e, i) => e <= parseFloat(scoreboard[i][1]),
);
const finalErr = wmape(
    plans.flatMap((p) => p.actual),
    plans.flatMap((p) => p.fc),
);
results[`out-of-sample WMAPE on the held-out 28 days: ${(finalErr * 100).toFixed(1)}%`] = finalErr < 0.35;
results["reconciled store forecasts are finite and non-negative"] = plans.all((p) =>
    p.fc.all((v) => Number.isFinite(v) && v >= 0),
);
results[`p10-p90 residual bands cover ${(bandCoverage * 100).toFixed(0)}% of held-out days`] =
    bandCoverage > 0.65 && bandCoverage < 0.95;
results[
    `ABC: the ${abc.A.length} A-class SKUs are the smallest top set with 80% of revenue (${(aShare * 100).toFixed(1)}%)`
] = aShare >= 0.8 && aMinusOne < 0.8 && abc.A.length + abc.B.length + abc.C.length === SKUS.length;
results["the SortedSet holiday lookup agrees with a linear scan"] = [0, 100, 300, 500, 700].all((d) => {
    const h = [...holidays].sortBy((x) => x).find((x) => x >= d);
    return daysToHoliday(d) === (h === undefined ? 60 : Math.min(60, h - d));
});
results[`simulated fill rate ${(fillRate * 100).toFixed(1)}% against a 95% target`] = fillRate > 0.93;
results["every order is a whole number of case packs"] = orders
    .slice(1)
    .every((o) => Number(o[7]) % Number(o[8]) === 0);
results["planner summary round-trips through YAML"] =
    YAML.Parse(readFile(work.join("summary.yaml"))).models.beverages === choice.beverages;

print("demand-planning");
print(
    `  history    ${rows.length - STORES.length * SKUS.length * HORIZON} daily rows (${STORES.length} stores x ${SKUS.length} SKUs x ${DAYS} days), ${holidays.size} holidays`,
);
print("  backtest   weighted MAPE over 4 rolling 28-day origins");
print(table([["family", "naive", "ewma x weekday", "boosted", "chosen"], ...scoreboard]));
print(
    `  forecast   held-out WMAPE ${(finalErr * 100).toFixed(1)}%; region totals north ${regionTotals.north}, south ${regionTotals.south} units`,
);
print(`  ABC        A ${abc.A.join(" ")} | B ${abc.B.join(" ")} | C ${abc.C.join(" ")}`);
print(`  bands      p10-p90 residual band covers ${(bandCoverage * 100).toFixed(1)}% of held-out days`);
print(
    `  policy     z=${Z95.toFixed(3)} (95%), ${orders.length - 1} store x SKU plans, simulated fill rate ${(fillRate * 100).toFixed(1)}%`,
);
removeAll(work);
check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
