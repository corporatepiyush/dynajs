// 15 · CSV sales report — loads a CSV export, aggregates by region and product, writes a report CSV.
//
// WHAT IT SHOWS
//   - dyna:csv CSVFile: reading a file-backed table with a header
//   - dyna:dataframe: typed columns, masks, group-bys and quantiles
//   - writing spreadsheet-safe CSV (formula injection is escaped)
//
// RUN      dynajs examples/apps/15-csv-sales-report.js [sales.csv] [report.csv]
//          Without arguments it generates a sample file in a temp directory.

import { CSVFile, stringify } from "dyna:csv";
import { DataFrame } from "dyna:dataframe";
import { Path, makeTempDir, writeFile, readFile, removeAll } from "dyna:file";

const argv = scriptArgs.slice(1);
const selfTest = argv.length === 0;
const work = selfTest ? makeTempDir("sales") : null;
const input = selfTest ? work.join("sales.csv") : new Path(argv[0]);
const output = selfTest ? work.join("report.csv") : new Path(argv[1] ?? "report.csv");

if (selfTest) {
    // A realistic export: mixed regions, a refund (negative units) and a
    // product name that a spreadsheet would execute as a formula.
    writeFile(input, [
        "order_id,region,product,units,unit_price",
        "1001,EMEA,Widget,10,19.99", "1002,EMEA,Gadget,2,149.00", "1003,APAC,Widget,25,19.99",
        "1004,AMER,Widget,5,19.99",  "1005,AMER,Gadget,7,149.00", "1006,APAC,Gizmo,40,4.50",
        "1007,EMEA,Widget,-3,19.99", "1008,AMER,=SUM(A1:A9),1,1.00", "1009,APAC,Gadget,3,149.00",
    ].join("\n") + "\n");
}

// ---- load ------------------------------------------------------------------
const table = new CSVFile(input).read();
const col = (name) => { const i = table.headers.indexOf(name); return table.rows.map((r) => r[i]); };

// CSV cells are strings; analytics want typed columns. Converting once here
// means every verb below runs over contiguous numbers.
const units = Int32Array.from(col("units"), Number);
const price = Float64Array.from(col("unit_price"), Number);
const revenue = Float64Array.from(units, (u, i) => u * price[i]);
const df = new DataFrame({ region: col("region"), product: col("product"), units, revenue });

// ---- analyze ---------------------------------------------------------------
const sales = df.GT("units", 0);                         // mask: exclude refunds
const byRegion = df.GROUP_BY_SUM("region", "revenue");   // refunds net out of revenue
const unitsByProduct = df.GROUP_BY_SUM("product", "units", sales);
const orderCount = df.GROUP_BY_COUNT("region");

const summary = {
    orders: df.ROWS,
    refunds: df.ROWS - sales.reduce((a, b) => a + b, 0),
    totalRevenue: df.SUM("revenue"),
    medianOrder: df.QUANTILE("revenue", 0.5, sales),
    largestOrder: df.MAX("revenue"),
};

// ---- write -----------------------------------------------------------------
const regionRows = byRegion.keys
    .map((region, i) => ({ region, revenue: byRegion.values[i], orders: orderCount.values[orderCount.keys.indexOf(region)] }))
    .sort((a, b) => b.revenue - a.revenue);

// Product names came from user input. A cell starting with = + - @ would run
// as a formula when the report is opened, so those cells get a leading quote.
const safe = (cell) => (/^[=+\-@]/.test(String(cell)) ? "'" + cell : cell);
const report = stringify([
    ["section", "key", "value"],
    ...regionRows.map((r) => ["revenue_by_region", r.region, r.revenue.toFixed(2)]),
    ...unitsByProduct.keys.map((p, i) => ["units_by_product", safe(p), String(unitsByProduct.values[i])]),
    ["summary", "total_revenue", summary.totalRevenue.toFixed(2)],
    ["summary", "median_order", summary.medianOrder.toFixed(2)],
]);
writeFile(output, report);
console.log(`read ${df.ROWS} orders, wrote ${String(output)}`);
for (const r of regionRows) console.log(`  ${r.region}: ${r.revenue.toFixed(2)} from ${r.orders} orders`);

// ---- self-test -------------------------------------------------------------
if (selfTest) {
    const check = (cond, what) => { if (!cond) throw new Error("self-test failed: " + what); };
    check(summary.orders === 9 && summary.refunds === 1, "nine orders, one refund");
    const emea = regionRows.find((r) => r.region === "EMEA").revenue;
    check(Math.abs(emea - (10 * 19.99 + 2 * 149 - 3 * 19.99)) < 1e-9, "EMEA revenue nets the refund");
    check(regionRows[0].region === "AMER", "regions are ranked by revenue");
    const widget = unitsByProduct.values[unitsByProduct.keys.indexOf("Widget")];
    check(widget === 40, "units by product exclude the refund row: " + widget);
    const written = readFile(output);
    check(written.includes("'=SUM(A1:A9)"), "the formula-shaped product name is neutralized");
    check(written.split("\n").length > 8, "the report has every section");
    console.log("self-test passed: total revenue", summary.totalRevenue.toFixed(2));
    removeAll(work);
}
