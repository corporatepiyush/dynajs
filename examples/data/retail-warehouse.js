// Retail warehouse — CSV extracts to a star schema, then revenue, margin, cohorts and basket metrics with exact money.
//
// WHAT IT IS
//   The nightly analytics job of a retailer, from raw extracts to the deliverables finance and growth read:
//
//   extract     four CSV exports (customers, products, orders, order lines) as a point-of-sale system writes them:
//               mixed-case codes, stray whitespace, a few lines referencing products that no longer exist
//   conform     clean and type the columns, build integer surrogate keys, quarantine orphan lines with reasons
//   model       a fact table (one row per order line) joined to customer and product dimensions as DataFrames
//   measure     revenue and gross margin by month x region (PIVOT), category mix, top products, average basket,
//               monthly cohort retention (customers by first-purchase month, active in each later month)
//   cohorts     one RoaringBitmap of active customers per month: a cohort's retention k months later is a single
//               andCardinality, never a per-customer Set probe
//   baskets     basket-size percentiles from a TDigest; market-basket affinity from one RoaringBitmap of order ids per
//               product — support(a, b) is andCardinality, lift = P(ab) / P(a)P(b) — ranked in a ZSet
//   RFM         recency, frequency and monetary value as Float32Array columns, z-scored and blended with dyna:simd
//               (normalize, axpy) and the top customers picked by topkIndices — no per-customer JS arithmetic
//   money       every currency total is summed in integer cents and checked against an exact Decimal ledger
//   publish     a YAML summary for the finance pack, the pivot as CSV, and an HTML dashboard page
//
// MODULES  dyna:csv · dyna:dataframe · dyna:decimal · dyna:time · dyna:yaml · dyna:html · dyna:random · dyna:file · dyna:cli
//          · dyna:structures (RoaringBitmap, TDigest, ZSet) · dyna:simd · String/Number/Date/Array extensions
//
// RUN      dynajs examples/data/retail-warehouse.js --in ./extracts --out ./reports     against real exports
//          dynajs examples/data/retail-warehouse.js                                     generates a year of trading

import { parse as parseCSV, stringify as toCSV } from "dyna:csv";
import { DataFrame } from "dyna:dataframe";
import { Decimal, Money } from "dyna:decimal";
import { parseDate } from "dyna:time";
import { Stringify as toYAML, Parse as fromYAML } from "dyna:yaml";
import { Template } from "dyna:html";
import { Random } from "dyna:random";
import { Path, makeTempDir, makeDir, readFile, writeFile, removeAll } from "dyna:file";
import { Command } from "dyna:cli";
import { RoaringBitmap, TDigest, ZSet } from "dyna:structures";
import * as simd from "dyna:simd";

const { options } = new Command("retail-warehouse")
    .option("--in <dir>", "directory with customers.csv, products.csv, orders.csv, lines.csv", { type: "string" })
    .option("--out <dir>", "report directory", { type: "string" })
    .parse(scriptArgs.slice(1));
const selfTest = !options.in;
const work = selfTest ? makeTempDir("warehouse") : null;
const inDir = selfTest ? work.join("extracts") : new Path(options.in);
const outDir = selfTest ? work.join("reports") : new Path(options.out || "reports");
makeDir(outDir, { recursive: true });
const ledger = selfTest ? generateExtracts(inDir) : null; // the exact totals the generator wrote, for verification

// =============================================================================
// Extract + conform
// =============================================================================
const read = (name) => parseCSV(readFile(inDir.join(name)), { strict: true });
const quarantine = [];
const tidy = (s) => s.compact();

const customersRaw = read("customers.csv");
const custKey = new Map();
const customers = { key: [], region: [], segment: [] };
for (const [id, name, region, segment] of customersRaw.rows) {
    custKey.set(id.trim().toUpperCase(), customers.key.length);
    customers.key.push(customers.key.length);
    customers.region.push(tidy(region).toUpperCase());
    customers.segment.push(tidy(segment).toLowerCase());
}
const productsRaw = read("products.csv");
const prodKey = new Map(),
    products = { sku: [], category: [], cost: [], name: [] };
for (const [sku, name, category, cost] of productsRaw.rows) {
    prodKey.set(sku.trim().toUpperCase(), products.sku.length);
    products.sku.push(sku.trim().toUpperCase());
    products.name.push(tidy(name));
    products.category.push(tidy(category).toLowerCase());
    products.cost.push(Money.fromString(cost.trim(), "USD").amount());
}
const orderInfo = new Map();
for (const [orderId, customerId, date] of read("orders.csv").rows) {
    const c = custKey.get(customerId.trim().toUpperCase());
    if (c === undefined) {
        quarantine.push({ file: "orders.csv", key: orderId, reason: `unknown customer ${customerId}` });
        continue;
    }
    const d = parseDate(date.trim());
    orderInfo.set(orderId.trim(), { c, month: d.year * 100 + d.month, day: d.epochDay });
}
const fact = { order: [], cust: [], prod: [], month: [], day: [], qty: [], cents: [], costCents: [] };
const exact = { revenue: Decimal.ZERO };
for (const [orderId, sku, qty, unitPrice] of read("lines.csv").rows) {
    const o = orderInfo.get(orderId.trim());
    const p = prodKey.get(sku.trim().toUpperCase());
    if (!o) {
        quarantine.push({ file: "lines.csv", key: orderId, reason: "order not found" });
        continue;
    }
    if (p === undefined) {
        quarantine.push({ file: "lines.csv", key: `${orderId}/${sku}`, reason: `discontinued product ${sku.trim()}` });
        continue;
    }
    const n = Number(qty),
        unit = Money.fromString(unitPrice.trim(), "USD");
    if (!Number.isInteger(n) || n <= 0) {
        quarantine.push({ file: "lines.csv", key: `${orderId}/${sku}`, reason: `bad quantity "${qty}"` });
        continue;
    }
    fact.order.push(Number(orderId.replace(/\D/g, "")));
    fact.cust.push(o.c);
    fact.prod.push(p);
    fact.month.push(o.month);
    fact.day.push(o.day);
    fact.qty.push(n);
    fact.cents.push(unit.mul(n).amount());
    fact.costCents.push(products.cost[p] * n);
    exact.revenue = exact.revenue.add(unit.mul(n).toDecimal());
}

// =============================================================================
// Model: fact + dimensions as DataFrames
// =============================================================================
const F = new DataFrame({
    order: Int32Array.from(fact.order),
    cust: Int32Array.from(fact.cust),
    prod: Int32Array.from(fact.prod),
    month: Int32Array.from(fact.month),
    day: Int32Array.from(fact.day),
    qty: Int32Array.from(fact.qty),
    cents: Float64Array.from(fact.cents),
    cost: Float64Array.from(fact.costCents),
});
const C = new DataFrame({ cust: Int32Array.from(customers.key), region: customers.region, segment: customers.segment });
const P = new DataFrame({ prod: Int32Array.from(products.sku.keys()), category: products.category, sku: products.sku });
const joined = F.JOIN(C, "cust", "cust", "inner").JOIN(P, "prod", "prod", "inner");
const wide = new DataFrame({ ...joined.TO_COLUMNS(), margin: joined.SUB("cents", "cost") });

const cents = (v) => new Money(Math.round(v), "USD").toString();
const pivot = wide.PIVOT("month", "region", "cents", "sum");
const marginPivot = wide.PIVOT("month", "region", "margin", "sum");
const byCategory = wide.GROUP_BY_SUM("category", "cents");
const unitsBySku = wide.GROUP_BY_SUM("sku", "qty");
const orders = new DataFrame({ order: Int32Array.from(fact.order), cents: Float64Array.from(fact.cents) });
const basket = orders.GROUP_BY_SUM("order", "cents");
const nonEmpty = basket.values.filter((v) => v > 0);

// Cohorts: first purchase month per customer; one RoaringBitmap of active customers per month index.
const firstMonth = wide.GROUP_BY_MIN("cust", "month");
const monthIndex = (m) => Math.floor(m / 100) * 12 + (m % 100) - 1;
const monthLabel = (idx) => new Date(Math.floor(idx / 12), idx % 12, 15).format("{Mon} {yyyy}");
const activeIn = new Map();
for (let i = 0; i < fact.cust.length; i++)
    activeIn.getOrInsertComputed(monthIndex(fact.month[i]), () => new RoaringBitmap()).add(fact.cust[i]);
const cohorts = new Map();
firstMonth.keys.forEach((cust, i) => {
    if (!Number.isNaN(firstMonth.values[i]))
        cohorts.getOrInsertComputed(monthIndex(firstMonth.values[i]), () => new RoaringBitmap()).add(cust);
});
const share = (members, idx) => (activeIn.has(idx) ? members.andCardinality(activeIn.get(idx)) : 0) / members.size;
const retention = [...cohorts.keys()]
    .sortBy((k) => k)
    .map((start) => {
        const members = cohorts.get(start);
        return {
            cohort: monthLabel(start),
            size: members.size,
            m1: share(members, start + 1),
            m3: share(members, start + 3),
        };
    });

// Basket sizes: TDigest percentiles over every order total.
const nonEmptyF64 = Float64Array.from(nonEmpty);
const basketDigest = new TDigest(200).addMany(nonEmptyF64);
const basketPct = Object.fromEntries(
    [0.5, 0.9, 0.99].map((q) => [`p${Math.round(q * 100)}`, basketDigest.quantile(q)]),
);

// Market basket: order-id bitmaps per product; support and lift by bitmap intersection.
const ordersOf = new Map();
for (let i = 0; i < fact.prod.length; i++)
    ordersOf.getOrInsertComputed(fact.prod[i], () => new RoaringBitmap()).add(fact.order[i]);
const orderCount = new RoaringBitmap(Int32Array.from(fact.order)).size;
const popular = [...ordersOf.keys()].sortBy((p) => -ordersOf.get(p).size).take(24);
const affinity = new ZSet();
for (let a = 0; a < popular.length; a++)
    for (let b = a + 1; b < popular.length; b++) {
        const A = ordersOf.get(popular[a]),
            B = ordersOf.get(popular[b]);
        const both = A.andCardinality(B);
        if (both < 5) continue;
        affinity.add(
            `${products.sku[popular[a]]}+${products.sku[popular[b]]}`,
            (both * orderCount) / (A.size * B.size),
        );
    }
const topPairs = affinity.range(0, 2, { reverse: true });

// RFM: columnar z-scores blended with simd; the top 10 by topkIndices.
const nCust = customers.key.length;
const lastDay = simd.i32Max(Int32Array.from(fact.day));
const recency = new Float32Array(nCust).fill(400),
    frequency = new Float32Array(nCust),
    monetary = new Float32Array(nCust);
for (let i = 0; i < fact.cust.length; i++) {
    const c = fact.cust[i];
    recency[c] = Math.min(recency[c], lastDay - fact.day[i]);
    monetary[c] += fact.cents[i] / 100;
}
const ordersByCust = new Map();
for (let i = 0; i < fact.cust.length; i++)
    ordersByCust.getOrInsertComputed(fact.cust[i], () => new RoaringBitmap()).add(fact.order[i]);
for (const [c, bm] of ordersByCust) frequency[c] = bm.size;
const rfm = simd.normalize(Float32Array.from(frequency));
simd.axpy(rfm, 1, simd.normalize(Float32Array.from(monetary)));
simd.axpy(rfm, -1, simd.normalize(Float32Array.from(recency)));
const vip = [...simd.topkIndices(rfm, 10)].sortBy((c) => -rfm[c]);

// =============================================================================
// Publish
// =============================================================================
const totalCents = wide.SUM("cents");
const summary = {
    period: `${Math.min(...fact.month)}..${Math.max(...fact.month)}`,
    revenue: cents(totalCents),
    grossMargin: cents(wide.SUM("margin")),
    marginPct: ((wide.SUM("margin") / totalCents) * 100).round(2),
    orders: nonEmpty.length,
    averageBasket: cents(simd.f64Mean(nonEmptyF64)),
    basketPercentiles: Object.fromEntries(Object.entries(basketPct).map(([k, v]) => [k, cents(v)])),
    affinity: topPairs.map(([pair, lift]) => ({ pair, lift: lift.round(2) })),
    vipCustomers: vip.map((c) => `c${c}`),
    revenueByCategory: Object.fromEntries(byCategory.keys.map((k, i) => [k, cents(byCategory.values[i])])),
    topProducts: unitsBySku.keys
        .zip([...unitsBySku.values])
        .sortBy(([, u]) => -u)
        .take(5)
        .map(([sku, units]) => ({ sku, units })),
    cohorts: retention.take(6).map((r) => ({ ...r, m1: (r.m1 * 100).round(1), m3: (r.m3 * 100).round(1) })),
    quarantined: quarantine.length,
};
writeFile(outDir.join("summary.yaml"), toYAML(summary, { sortKeys: false }));
const regions = pivot.COLUMNS.filter((c) => c !== "month");
const pivotCols = pivot.TO_COLUMNS();
const pivotRows = [["month", ...regions, "total"]];
for (let i = 0; i < pivot.ROWS; i++) {
    const vals = regions.map((r) => pivotCols[r][i] || 0);
    pivotRows.push([String(pivotCols.month[i]), ...vals.map(cents), cents(vals.reduce((a, b) => a + b, 0))]);
}
writeFile(outDir.join("revenue-by-month-region.csv"), toCSV(pivotRows));
writeFile(outDir.join("quarantine.csv"), toCSV(quarantine));
writeFile(
    outDir.join("dashboard.html"),
    new Template(`<!doctype html><title>Retail dashboard</title>
<h1>Revenue {{revenue}} · margin {{marginPct}}%</h1><p>{{orders}} orders, average basket {{averageBasket}}</p>
<table>{{#rows}}<tr>{{#.}}<td>{{.}}</td>{{/.}}</tr>{{/rows}}</table>`).render({ ...summary, rows: pivotRows }),
);

print("retail-warehouse");
print(
    `  facts        ${wide.ROWS} order lines, ${C.ROWS} customers, ${P.ROWS} products, ${quarantine.length} lines quarantined`,
);
print(
    `  revenue      ${summary.revenue}, margin ${summary.grossMargin} (${summary.marginPct}%), basket ${summary.averageBasket}`,
);
print(
    `  categories   ${Object.entries(summary.revenueByCategory)
        .map(([k, v]) => `${k} ${v}`)
        .join(", ")}`,
);
print(
    `  cohorts      ${summary.cohorts
        .map((c) => `${c.cohort}: n=${c.size} m1=${c.m1}% m3=${c.m3}%`)
        .take(3)
        .join(" | ")}`,
);
print(
    `  baskets      p50 ${summary.basketPercentiles.p50}, p90 ${summary.basketPercentiles.p90}, p99 ${summary.basketPercentiles.p99}`,
);
print(`  affinity     ${summary.affinity.map((a) => `${a.pair} lift ${a.lift}`).join(" | ")}`);
print(`  VIP (RFM)    ${summary.vipCustomers.take(5).join(" ")}`);
print(`  reports      ${outDir}`);

if (selfTest) {
    const results = {};
    results["DataFrame cents total equals the exact Decimal ledger"] =
        Money.fromDecimal(exact.revenue, "USD").amount() === totalCents;
    results[`ledger revenue ${exact.revenue} equals what the source system recorded ${ledger.revenue}`] =
        exact.revenue.equals(ledger.revenue);
    results[`orphans quarantined with reasons (${quarantine.length})`] =
        quarantine.length === ledger.orphans &&
        quarantine.every((q) => /discontinued|not found|bad quantity|unknown customer/.test(q.reason));
    const pivotTotal = pivotRows
        .drop(1)
        .reduce((s, r) => s.add(Money.fromString(r.last(), "USD")), new Money(0, "USD"));
    results["the month x region pivot sums to the revenue total"] = pivotTotal.amount() === totalCents;
    results["cohort shares are proportions"] = retention.every((r) => r.m1 >= 0 && r.m1 <= 1) && retention[0].size > 0;
    const cohortSizes = retention.pluck("size").sum();
    results["every buying customer is in exactly one cohort bitmap"] =
        cohortSizes === new RoaringBitmap(Int32Array.from(fact.cust)).size;
    const sorted = nonEmptyF64.slice().sort();
    const exactP90 = sorted[Math.floor(0.9 * (sorted.length - 1))];
    results["TDigest p90 basket within 2% of the exact order statistic"] =
        Math.abs(basketPct.p90 - exactP90) / exactP90 < 0.02;
    results[`the planted cross-sell pair is the top lift (${topPairs[0]?.[0]})`] =
        topPairs[0]?.[0]
            .split("+")
            .sortBy((x) => x)
            .join("+") === ledger.plantedPair;
    const medianFreq = [...frequency].filter((f) => f > 0).median();
    results["RFM VIPs are frequent buyers"] = vip.all((c) => frequency[c] >= medianFreq);
    results["YAML summary round-trips"] = fromYAML(readFile(outDir.join("summary.yaml"))).revenue === summary.revenue;
    removeAll(work);
    check(results);
}

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}

// ---- a year of trading, written the way a point-of-sale system exports it ----------
function generateExtracts(dir) {
    makeDir(dir, { recursive: true });
    const rng = new Random(2026);
    const regions = ["north", "South ", " east", "WEST"];
    const custRows = [["customer_id", "name", "region", "segment"]];
    for (let i = 0; i < 400; i++)
        custRows.push([`c${i}`, `Customer ${i}`, rng.choice(regions), rng.choice(["Retail", "business ", "VIP"])]);
    const cats = { apparel: [15, 90], electronics: [40, 900], grocery: [2, 30], home: [10, 250] };
    const prodRows = [["sku", "name", "category", "unit_cost"]],
        prices = [];
    let sku = 0;
    for (const [cat, [lo, hi]] of Object.entries(cats))
        for (let j = 0; j < 15; j++) {
            const price = lo + rng.nextBounded((hi - lo) * 100) / 100;
            prices.push([`sk-${sku}`, price]);
            prodRows.push([
                `SK-${sku}`,
                `  ${cat} item ${j}`,
                cat,
                (price * (0.45 + rng.nextFloat() * 0.3)).toFixed(2),
            ]);
            sku++;
        }
    const orderRows = [["order_id", "customer_id", "date"]],
        lineRows = [["order_id", "sku", "qty", "unit_price"]];
    let revenue = Decimal.ZERO,
        orphans = 0;
    for (let o = 0; o < 3000; o++) {
        const cust = Math.floor(Math.abs(rng.normal(200, 120))) % 400;
        const month = 1 + Math.min(11, Math.floor(rng.exponential(0.25) + (cust % 6)));
        orderRows.push([
            `o${o}`,
            `C${cust}`,
            `2025-${String(month).padStart(2, "0")}-${String(1 + rng.nextBounded(28)).padStart(2, "0")}`,
        ]);
        for (let l = 0, n = 1 + rng.poisson(1.5); l < n; l++) {
            const [s, p] = rng.choice(prices);
            const qty = 1 + rng.poisson(0.6);
            if (rng.nextFloat() < 0.01) {
                lineRows.push([`o${o}`, "SK-9999", String(qty), "9.99"]);
                orphans++;
                continue;
            }
            lineRows.push([`o${o}`, s, String(qty), p.toFixed(2)]);
            revenue = revenue.add(new Decimal(p.toFixed(2)).mul(qty));
            if (s === "sk-3" && rng.nextFloat() < 0.8) {
                lineRows.push([`o${o}`, "sk-41", "1", prices[41][1].toFixed(2)]); // the planted cross-sell
                revenue = revenue.add(new Decimal(prices[41][1].toFixed(2)));
            }
        }
    }
    writeFile(dir.join("customers.csv"), toCSV(custRows));
    writeFile(dir.join("products.csv"), toCSV(prodRows));
    writeFile(dir.join("orders.csv"), toCSV(orderRows));
    writeFile(dir.join("lines.csv"), toCSV(lineRows));
    return { revenue, orphans, plantedPair: "SK-3+SK-41" };
}
