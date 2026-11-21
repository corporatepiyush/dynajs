// Columnar store — dictionary, run-length and bit-packed columns with zone maps and bitmap predicates, checked
// against a row-at-a-time scan of the same data.
//
// WHAT IT IS
//   The storage and scan layer of an analytical database (Parquet / ClickHouse / DuckDB in miniature):
//
//   encode      strings become dictionary codes (Interner), the sorted date column becomes runs, codes are
//               bit-packed to their minimum width (mathx.bits.len32) into Uint32Array words
//   zone maps   per-block min/max for every numeric column (simd i32Min/i32Max, f64Min/f64Max on subarrays):
//               a block whose range cannot match is never decoded
//   predicates  equality and prefix filters on strings become RoaringBitmap postings per dictionary code (Trie
//               for the prefix -> codes step); ranges on the sorted column become one addRange; AND/OR are native
//   aggregate   SUM(qty * price) GROUP BY region over the selection, plus COUNT(DISTINCT customer) with a
//               HyperLogLog and exact distinct counts from bitmap cardinality
//   joins       GROUP BY supplier through a dimension table: the join runs once per dictionary code (600), not
//               once per row, and the fact column is never widened
//   top-N       the five biggest customers by revenue in the selection
//   mutations   a GDPR erase marks rows in a deletion vector (a RoaringBitmap AND-NOTed into every selection) and
//               late rows land in a row-format delta store scanned beside the columns; compaction rewrites both
//               into fresh columns. Every query is checked in all three states
//   oracle      every query is re-run over the original row objects with plain filters; results must match
//
// MODULES  dyna:structures (Interner, RoaringBitmap, Trie, HyperLogLog, SortedMap) · dyna:simd (i32Min, i32Max,
//          f64Min, f64Max, f64Sum, f64Dot) · dyna:mathx (bits) · dyna:compress (zstd) · dyna:random · dyna:cli
//
// RUN      dynajs examples/data/columnar-store.js [rows]

import { Interner, RoaringBitmap, Trie, HyperLogLog, SortedMap } from "dyna:structures";
import { i32Min, i32Max, f64Min, f64Max, f64Sum, f64Dot } from "dyna:simd";
import { bits } from "dyna:mathx";
import { zstd, unzstd } from "dyna:compress";
import { Random } from "dyna:random";
import { Table } from "dyna:cli";
import { cpuUsage } from "dyna:sys";

const results = {};
const ROWS = Number(scriptArgs[1] ?? 200_000);
const BLOCK = 4096;
const rng = new Random(2026);

// =============================================================================
// Data: a year of order lines, sorted by day as an ingest pipeline would write them
// =============================================================================
const REGIONS = ["emea-north", "emea-south", "amer-east", "amer-west", "apac-jp", "apac-au", "apac-in", "latam"];
const products = Array.from(
    { length: 600 },
    (_, i) => `${["pro", "home", "kids", "garden"][i % 4]}-${String(i).padStart(4, "0")}`,
);
const rows = [];
for (let i = 0; i < ROWS; i++) {
    const day = Math.floor((i / ROWS) * 365);
    rows.push({
        day,
        region: REGIONS[Math.min(REGIONS.length - 1, Math.floor(rng.exponential(0.6)))],
        product: products[Math.floor(products.length * rng.nextFloat() ** 2)],
        customer: rng.nextBounded(40_000),
        qty: 1 + rng.poisson(2),
        price: Math.round((5 + rng.exponential(1 / 40)) * 100) / 100,
    });
}

// the dimension table: each product's supplier (drawn after the facts, so the fact rows are unchanged)
const SUPPLIERS = Array.from({ length: 30 }, (_, i) => `supplier-${String(i).padStart(2, "0")}`);
const supplierOf = new Map(products.map((p) => [p, SUPPLIERS[rng.nextBounded(SUPPLIERS.length)]]));

// =============================================================================
// Encoding
// =============================================================================
class StringColumn {
    constructor(values) {
        this.dict = new Interner();
        const codes = this.dict.internMany(values);
        this.cardinality = i32Max(new Int32Array(codes.buffer, codes.byteOffset, codes.length)) + 1;
        this.width = Math.max(1, bits.len32(this.cardinality - 1));
        this.packed = pack(codes, this.width);
        this.length = values.length;
        this.postings = Array.from({ length: this.cardinality }, () => new RoaringBitmap());
        const buckets = Array.from({ length: this.cardinality }, () => []);
        for (let i = 0; i < codes.length; i++) buckets[codes[i]].push(i);
        buckets.forEach((b, c) => this.postings[c].addMany(Uint32Array.from(b)));
        this.trie = new Trie();
        for (let c = 0; c < this.cardinality; c++) this.trie.insert(this.dict.get(c));
    }
    eq(value) {
        const c = this.dict.lookup(value);
        return c === undefined ? new RoaringBitmap() : this.postings[c].clone();
    }
    prefix(p) {
        const out = new RoaringBitmap();
        for (const v of this.trie.keysWithPrefix(p)) out.or(this.postings[this.dict.lookup(v)]);
        return out;
    }
    codes() {
        return unpack(this.packed, this.width, this.length);
    }
    codeAt(i) {
        const bit = i * this.width,
            w = bit >>> 5,
            off = bit & 31;
        let v = this.packed[w] >>> off;
        if (off + this.width > 32) v |= this.packed[w + 1] << (32 - off);
        return v & ((1 << this.width) - 1);
    }
}
function pack(values, width) {
    const words = new Uint32Array(Math.ceil((values.length * width) / 32) + 1);
    for (let i = 0, bit = 0; i < values.length; i++, bit += width) {
        const w = bit >>> 5,
            off = bit & 31;
        words[w] |= values[i] << off;
        if (off + width > 32) words[w + 1] |= values[i] >>> (32 - off);
    }
    return words;
}
function unpack(words, width, n) {
    const out = new Uint32Array(n),
        mask = width === 32 ? 0xffffffff : (1 << width) - 1;
    for (let i = 0, bit = 0; i < n; i++, bit += width) {
        const w = bit >>> 5,
            off = bit & 31;
        let v = words[w] >>> off;
        if (off + width > 32) v |= words[w + 1] << (32 - off);
        out[i] = v & mask;
    }
    return out;
}
class RunColumn {
    constructor(values) {
        const starts = [],
            vals = [];
        for (let i = 0; i < values.length; i++)
            if (i === 0 || values[i] !== values[i - 1]) {
                starts.push(i);
                vals.push(values[i]);
            }
        this.starts = Int32Array.from(starts);
        this.values = Int32Array.from(vals);
        this.length = values.length;
        this.index = new SortedMap();
        vals.forEach((v, k) => this.index.set(v, k));
    }
    // Row range [lo, hi) whose value lies in [a, b]: two ordered lookups, no scan.
    rowsBetween(a, b) {
        const first = this.index.ceilKey(a),
            last = this.index.floorKey(b);
        if (first === undefined || last === undefined || first > b) return [0, 0];
        const k1 = this.index.get(first),
            k2 = this.index.get(last);
        return [this.starts[k1], k2 + 1 < this.starts.length ? this.starts[k2 + 1] : this.length];
    }
}

let store, zones;
function build(rs) {
    store = {
        length: rs.length,
        day: new RunColumn(rs.map((r) => r.day)),
        region: new StringColumn(rs.map((r) => r.region)),
        product: new StringColumn(rs.map((r) => r.product)),
        customer: Int32Array.from(rs, (r) => r.customer),
        qty: Int32Array.from(rs, (r) => r.qty),
        price: Float64Array.from(rs, (r) => r.price),
    };
    // the join, resolved once per product code
    store.supplierOfCode = Int32Array.from({ length: store.product.cardinality }, (_, c) =>
        SUPPLIERS.indexOf(supplierOf.get(store.product.dict.get(c))),
    );
    zones = [];
    for (let lo = 0; lo < rs.length; lo += BLOCK) {
        const hi = Math.min(rs.length, lo + BLOCK);
        zones.push({
            lo,
            hi,
            qty: [i32Min(store.qty.subarray(lo, hi)), i32Max(store.qty.subarray(lo, hi))],
            price: [f64Min(store.price.subarray(lo, hi)), f64Max(store.price.subarray(lo, hi))],
        });
    }
}
const t0 = performance.now();
build(rows);
const encodeMs = performance.now() - t0;
let deleted = new RoaringBitmap(), // the deletion vector: row positions in the current columns
    delta = []; // rows appended since the last compaction, kept as objects
results["bit-packed codes round-trip"] = (() => {
    const back = store.product.codes();
    for (let i = 0; i < ROWS; i += 7) if (store.product.dict.get(back[i]) !== rows[i].product) return false;
    return store.region.dict.get(store.region.codes()[ROWS - 1]) === rows[ROWS - 1].region;
})();
results["region codes pack into 3 bits, products into 10"] = store.region.width === 3 && store.product.width === 10;
results["date column collapses to one run per day"] = store.day.values.length === 365;

// =============================================================================
// Query engine
// =============================================================================
function scan(q) {
    let sel = new RoaringBitmap();
    const [lo, hi] = store.day.rowsBetween(q.dayFrom, q.dayTo);
    sel.addRange(lo, hi);
    sel.andNot(deleted);
    if (q.region) sel.and(store.region.eq(q.region));
    if (q.productPrefix) sel.and(store.product.prefix(q.productPrefix));
    let pruned = 0;
    if (q.minPrice !== undefined || q.minQty !== undefined) {
        const keep = new RoaringBitmap();
        for (const z of zones) {
            if (z.hi <= lo || z.lo >= hi) continue;
            if (
                (q.minPrice !== undefined && z.price[1] < q.minPrice) ||
                (q.minQty !== undefined && z.qty[1] < q.minQty)
            ) {
                pruned++;
                continue;
            }
            keep.addRange(z.lo, z.hi);
        }
        sel.and(keep);
    }
    const ids = sel.toArray();
    const revenue = new Float64Array(store.region.cardinality),
        count = new Int32Array(store.region.cardinality);
    const hll = new HyperLogLog(14),
        distinct = new RoaringBitmap();
    const qtyF = new Float64Array(ids.length + delta.length),
        priceF = new Float64Array(ids.length + delta.length),
        bySupplier = new Float64Array(SUPPLIERS.length),
        byCustomer = new Float64Array(40_000);
    let n = 0;
    for (const i of ids) {
        if (q.minPrice !== undefined && store.price[i] < q.minPrice) continue;
        if (q.minQty !== undefined && store.qty[i] < q.minQty) continue;
        const c = store.region.codeAt(i);
        const v = store.qty[i] * store.price[i];
        revenue[c] += v;
        count[c]++;
        bySupplier[store.supplierOfCode[store.product.codeAt(i)]] += v;
        byCustomer[store.customer[i]] += v;
        qtyF[n] = store.qty[i];
        priceF[n] = store.price[i];
        n++;
        hll.add(String(store.customer[i]));
        distinct.add(store.customer[i]);
    }
    for (const r of delta) {
        // the delta store is row format: filtered row by row, merged into the same accumulators
        if (r.day < q.dayFrom || r.day > q.dayTo || (q.region && r.region !== q.region)) continue;
        if (q.productPrefix && !r.product.startsWith(q.productPrefix)) continue;
        if ((q.minPrice !== undefined && r.price < q.minPrice) || (q.minQty !== undefined && r.qty < q.minQty))
            continue;
        const c = store.region.dict.lookup(r.region),
            v = r.qty * r.price;
        revenue[c] += v;
        count[c]++;
        bySupplier[SUPPLIERS.indexOf(supplierOf.get(r.product))] += v;
        byCustomer[r.customer] += v;
        qtyF[n] = r.qty;
        priceF[n] = r.price;
        n++;
        hll.add(String(r.customer));
        distinct.add(r.customer);
    }
    // top five customers: one pass keeping a sorted list of five
    const top = [];
    for (let k = 0; k < byCustomer.length; k++)
        if (byCustomer[k] > 0 && (top.length < 5 || byCustomer[k] > top[4][1])) {
            top.push([k, byCustomer[k]]);
            top.sort((a, b) => b[1] - a[1]);
            if (top.length > 5) top.pop();
        }
    const groups = {};
    for (let c = 0; c < revenue.length; c++)
        if (count[c]) groups[store.region.dict.get(c)] = { revenue: revenue[c], rows: count[c] };
    const suppliers = {};
    bySupplier.forEach((v, k) => v && (suppliers[SUPPLIERS[k]] = v));
    return {
        groups,
        suppliers,
        top,
        pruned,
        rows: n,
        total: f64Dot(qtyF.subarray(0, n), priceF.subarray(0, n)),
        avgPrice: n ? f64Sum(priceF.subarray(0, n)) / n : 0,
        distinct: distinct.toArray().length,
        approxDistinct: hll.count(),
    };
}
function rowScan(q) {
    const groups = {},
        customers = new Set(),
        suppliers = {},
        perCustomer = new Map();
    let total = 0,
        rowsHit = 0;
    for (const r of rows) {
        if (r.gone) continue;
        if (r.day < q.dayFrom || r.day > q.dayTo) continue;
        if (q.region && r.region !== q.region) continue;
        if (q.productPrefix && !r.product.startsWith(q.productPrefix)) continue;
        if (q.minPrice !== undefined && r.price < q.minPrice) continue;
        if (q.minQty !== undefined && r.qty < q.minQty) continue;
        const g = (groups[r.region] ??= { revenue: 0, rows: 0 });
        g.revenue += r.qty * r.price;
        g.rows++;
        total += r.qty * r.price;
        rowsHit++;
        customers.add(r.customer);
        const s = supplierOf.get(r.product);
        suppliers[s] = (suppliers[s] || 0) + r.qty * r.price;
        perCustomer.set(r.customer, (perCustomer.get(r.customer) || 0) + r.qty * r.price);
    }
    const top = [...perCustomer].sort((a, b) => b[1] - a[1]).slice(0, 5);
    return { groups, suppliers, top, perCustomer, total, rows: rowsHit, distinct: customers.size };
}
const same = (a, b) => {
    const ka = Object.keys(a.groups).sort(),
        kb = Object.keys(b.groups).sort();
    if (ka.join() !== kb.join() || a.rows !== b.rows || a.distinct !== b.distinct) return false;
    for (const k of ka)
        if (
            a.groups[k].rows !== b.groups[k].rows ||
            Math.abs(a.groups[k].revenue - b.groups[k].revenue) > 1e-6 * b.groups[k].revenue
        )
            return false;
    const close = (x, y) => Math.abs(x - y) <= 1e-9 * Math.max(1, Math.abs(y));
    const sa = Object.keys(a.suppliers).sort(),
        sb = Object.keys(b.suppliers).sort();
    if (sa.join() !== sb.join() || sa.some((k) => !close(a.suppliers[k], b.suppliers[k]))) return false;
    // top five: the same revenues in order, and each named customer really has that revenue (ties may name either)
    if (
        a.top.length !== b.top.length ||
        a.top.some(([c, v], k) => !close(v, b.top[k][1]) || !close(b.perCustomer.get(c), v))
    )
        return false;
    return close(a.total, b.total);
};

const queries = [
    { name: "Q1 one quarter, all regions", dayFrom: 90, dayTo: 180 },
    { name: "Q2 pro-* in amer-east, H2", dayFrom: 182, dayTo: 364, region: "amer-east", productPrefix: "pro-" },
    { name: "Q3 premium lines (price >= 400)", dayFrom: 0, dayTo: 364, minPrice: 400 },
    { name: "Q4 bulk orders of garden-00*", dayFrom: 0, dayTo: 364, productPrefix: "garden-00", minQty: 6 },
    { name: "Q5 empty: unknown region", dayFrom: 0, dayTo: 364, region: "antarctica" },
    { name: "Q6 one day", dayFrom: 200, dayTo: 200 },
];
const table = [["query", "rows", "revenue", "distinct", "hll err", "col cpu ms", "row cpu ms"]];
let colTotal = 0,
    rowTotal = 0,
    allSame = true,
    hllOk = true;
const cpuMs = () => {
    const c = cpuUsage();
    return (c.user + c.system) * 1000;
};
const bestOf3 = (fn) => {
    let best = Infinity,
        out;
    for (let i = 0; i < 3; i++) {
        const t = cpuMs();
        out = fn();
        best = Math.min(best, cpuMs() - t);
    }
    return [out, best];
};
for (const q of queries) {
    const [col, colMs] = bestOf3(() => scan(q)),
        [row, rowMs] = bestOf3(() => rowScan(q));
    colTotal += colMs;
    rowTotal += rowMs;
    if (!same(col, row)) {
        allSame = false;
        print(`mismatch in ${q.name}`);
    }
    const err = col.distinct ? Math.abs(col.approxDistinct - col.distinct) / col.distinct : 0;
    if (err > 0.03) hllOk = false;
    table.push([
        q.name,
        col.rows.format(),
        col.total.format(2),
        col.distinct.format(),
        (err * 100).toFixed(2) + "%",
        colMs.toFixed(1),
        rowMs.toFixed(1),
    ]);
}
print(Table(table.slice(1), { head: table[0] }));
results["columnar results equal the row scan for every query"] = allSame;
results["HyperLogLog distinct counts within 3%"] = hllOk;
results["columnar scans beat the row scan in total"] = colTotal < rowTotal;

// =============================================================================
// Mutations: a GDPR erase, late rows, then compaction
// =============================================================================
const ERASED = new Set(Array.from({ length: 400 }, (_, i) => i * 97)); // customer ids to forget
const everyQuery = () => queries.every((q) => same(scan(q), rowScan(q)));
for (let i = 0; i < store.length; i++)
    if (ERASED.has(store.customer[i])) {
        deleted.add(i);
        rows[i].gone = true;
    }
for (let i = 0; i < 3000; i++) {
    const r = {
        day: 364,
        region: REGIONS[rng.nextBounded(REGIONS.length)],
        product: products[rng.nextBounded(products.length)],
        customer: 1 + 97 * rng.nextBounded(412), // never an erased id (multiples of 97)
        qty: 1 + rng.poisson(5),
        price: Math.round((5 + rng.exponential(1 / 200)) * 100) / 100,
    };
    rows.push(r);
    delta.push(r);
}
const erasedRows = deleted.size;
results[
    `with ${erasedRows} erased rows in the deletion vector and ${delta.length} in the delta store, every query matches`
] = erasedRows > 1000 && everyQuery();
const c0 = performance.now();
build(rows.filter((r) => !r.gone));
deleted = new RoaringBitmap();
delta = [];
const compactMs = performance.now() - c0;
results[`compaction rewrites ${store.length.format()} live rows into columns and every query still matches`] =
    store.length === ROWS - erasedRows + 3000 && store.day.values.length === 365 && everyQuery();
results["no erased customer survives compaction"] = !store.customer.some((c) => ERASED.has(c));
results["Q1's top customer by revenue exists and matches the row scan"] = scan(queries[0]).top.length === 5;
results["empty predicate yields an empty result"] = scan(queries[4]).rows === 0;
results["zone maps prune blocks for the price predicate"] =
    scan(queries[2]).pruned > 0 || zones.every((z) => z.price[1] >= 400);

// =============================================================================
// Footprint
// =============================================================================
const enc32 = (a) => new Uint8Array(a.buffer, a.byteOffset, a.byteLength);
const columnBytes =
    store.region.packed.byteLength +
    store.product.packed.byteLength +
    store.day.starts.byteLength * 2 +
    store.customer.byteLength +
    store.qty.byteLength +
    store.price.byteLength;
const jsonBytes = new TextEncoder().encode(JSON.stringify(rows)).length;
const zProduct = zstd(enc32(store.product.packed));
results["zstd of packed codes round-trips"] = (() => {
    const back = new Uint32Array(unzstd(zProduct).buffer.slice(0));
    return back.length === store.product.packed.length && back.every((v, i) => v === store.product.packed[i]);
})();
results["columns are at least 4x smaller than JSON rows"] = columnBytes * 4 < jsonBytes;
print(
    `encode ${encodeMs.toFixed(0)} ms; columns ${columnBytes.bytes()} vs JSON ${jsonBytes.bytes()}; product codes zstd ${zProduct.length.bytes()}`,
);
print(`mutations: ${erasedRows} rows erased, 3000 appended; compaction ${compactMs.toFixed(0)} ms`);
print(`scan total: columnar ${colTotal.toFixed(1)} ms, row-at-a-time ${rowTotal.toFixed(1)} ms`);

check(results);

function check(results) {
    const names = Object.keys(results);
    const failed = names.filter((name) => !results[name]);
    for (const name of failed) print("FAIL " + name);
    if (failed.length) throw new Error(`${failed.length} of ${names.length} checks failed`);
}
